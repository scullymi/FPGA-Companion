/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.c
 *  @brief The achievement set of the current game.
 *
 *  The conditions belong to RetroAchievements and are not compiled into the
 *  firmware. The set lies on the card as RA_PATCH_FILE, the server's reply to
 *  r=patch, and rcheevos' own parser reads it (rc_api_runtime.c). Nothing here
 *  interprets the JSON. Titles are copied into a small table, the parsed
 *  conditions live inside rcheevos. Nothing in this firmware writes the file
 *  yet, it is put on the card from outside. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "rc_api_runtime.h"
#include <ff.h>
#include "debug.h"
#include "sdc.h"
#include "ra_patch.h"

/* The game, fixed for now: Galaga on RetroAchievements, and the hash the server
   knows it by, md5("galaga"), the name of the arcade ROM set. The rest of the
   code asks ra_game_id() and ra_game_hash(), so the game's identity is set only
   here. */
#define RA_PATCH_GAME_ID    12138u                               /**< id on the server */
#define RA_PATCH_GAME_HASH  "b8140b5e33c53b0f7dd3cc368951a4dd"   /**< md5 of the ROM set name */
// the switches the set expects: 3 lives (list 'L' value 2) and bonus at 20K/70K
// (list 'B' value 2), the listentry values of the core's XML. Most of the
// conditions read these settings from the game's RAM, so with other ones they
// never fire, without a word.
static const ra_dip_t game_dips[] = { { 'L', 2 }, { 'B', 2 } };

#define RA_PATCH_BODY_MAX   40960        /**< a whole set, e.g. Galaga 15757 bytes (24.09.2026) */
#define RA_PATCH_MAX        64           /**< achievements kept per set, e.g. Galaga 17 */
#define RA_PATCH_TITLE_MAX  32           /**< titles are cut to 31 characters, the FPGA banner will show 24 */
#define RA_PATCH_FILE       "/sd/ra_patch.json"   /**< the server's reply to r=patch, kept on the card */
/** "Warning: Unknown Emulator", which the server adds for clients it does not
   know. Not an achievement of the game. */
#define RA_PATCH_WARNING_ID 101000001u

// the set file as it was read, for rcheevos' parser. Static, 40 KB would not
// fit on a task's stack.
static char     body[RA_PATCH_BODY_MAX];
static unsigned body_len;

// the achievements that are active: the title for the log, the 1-based position
// for the FPGA back channel and the RA task. rcheevos only reports an id when
// one fires.
static struct {
  unsigned id;                        /**< achievement id on the server */
  char     title[RA_PATCH_TITLE_MAX];  /**< its title, cut to fit */
} set[RA_PATCH_MAX];
static unsigned set_n;

const char *ra_game_hash(void) { return RA_PATCH_GAME_HASH; }
unsigned    ra_game_id(void)   { return RA_PATCH_GAME_ID; }
const ra_dip_t *ra_game_dips(unsigned *n) {
  *n = sizeof(game_dips) / sizeof(game_dips[0]);
  return game_dips;
}

unsigned ra_patch_count(void) { return set_n; }

// 1-based, so that 0 can mean "not in the set". A plain search, the table
// holds a few dozen entries at most.
unsigned ra_patch_index(unsigned id) {
  unsigned i;
  for(i = 0; i < set_n; i++) if(set[i].id == id) return i + 1;
  return 0;
}

const char *ra_patch_title(unsigned id) {
  unsigned i = ra_patch_index(id);
  return i ? set[i - 1].title : "";
}

/* Reads the set into body and says why when it cannot. A set that does not fit
   with one byte to spare is refused whole: cut short it would only show up as
   "no valid JSON". */
static bool card_read(void) {
  FIL f;
  UINT got = 0;
  FSIZE_t size = 0;
  FRESULT r;

  // the card is shared with other tasks, only the file access runs under the lock
  sdc_lock();
  r = f_open(&f, RA_PATCH_FILE, FA_READ);
  if(r == FR_OK) {
    size = f_size(&f);
    // read only a set that fits whole, one byte stays free for the final NUL
    if(size && size < sizeof(body)) r = f_read(&f, body, (UINT)size, &got);
    f_close(&f);
  }
  sdc_unlock();

  // one log line per failure: no file, empty, too large, and last any other open
  // or read error (FatFs returns the whole file with FR_OK, got != size only guards)
  if(r == FR_NO_FILE || r == FR_NO_PATH)
    debugf("RA: no set on the card (%s), no achievements", RA_PATCH_FILE);
  else if(r == FR_OK && !size)
    debugf("RA: set on the card is empty, no achievements");
  else if(size >= sizeof(body))
    debugf("RA: set on the card is %lu bytes, only %u fit, no achievements",
           (unsigned long)size, (unsigned)sizeof(body) - 1);
  else if(r != FR_OK || got != size)
    debugf("RA: set on the card not read (error %d, %u of %lu bytes), no achievements",
           (int)r, (unsigned)got, (unsigned long)size);
  else {
    body[got] = 0;              // a guard only, rcheevos reads body_len bytes
    body_len = got;
    return true;
  }
  return false;
}

/* Only the official achievements count. The set also holds unofficial ones
   (category 5, not finished or under review) and the server's pseudo
   achievement RA_PATCH_WARNING_ID, both are left out. */
static bool core_item(const rc_api_achievement_definition_t *a) {
  return a->category == RC_ACHIEVEMENT_CATEGORY_CORE && a->id != RA_PATCH_WARNING_ID;
}

/* Lets rcheevos parse the body, prints why when it is unusable. The caller
   destroys r in every case, as rc_api asks. */
static int parse_body(rc_api_fetch_game_data_response_t *r) {
  // the file is the server's reply to r=patch, so it is handed to the parser as
  // if it had just arrived with HTTP 200
  rc_api_server_response_t sr;
  memset(&sr, 0, sizeof(sr));
  sr.body = body;
  sr.body_length = body_len;
  sr.http_status_code = 200;
  // unusable: not JSON or broken JSON, a required field missing, no memory, or a
  // reply the server marked as failed. The server's text, else rcheevos' own.
  int rv = rc_api_process_fetch_game_data_server_response(r, &sr);
  if(rv != RC_OK || !r->response.succeeded) {
    debugf("RA: set unusable: %s", r->response.error_message ? r->response.error_message :
           rv != RC_OK ? rc_error_str(rv) : "marked as failed");
    return -1;
  }
  // a set of another game on the card would watch the wrong addresses
  if(r->id != ra_game_id()) {
    debugf("RA: set is for game %u, not %u", (unsigned)r->id, ra_game_id());
    return -1;
  }
  return 0;
}

/* Activates the core achievements and fills the title table. A condition that
   rcheevos rejects is reported, it would otherwise never fire in silence. */
static int activate_set(rc_runtime_t *rt, const rc_api_fetch_game_data_response_t *r) {
  unsigned i, rejected = 0;

  // every achievement of the set once: skip what does not count, stop when the
  // table is full, and hand the condition string of the others to rcheevos.
  // Stopping keeps rcheevos and the table in step: an achievement active without
  // a table entry would fire without title and position.
  set_n = 0;
  for(i = 0; i < r->num_achievements; i++) {
    const rc_api_achievement_definition_t *a = &r->achievements[i];
    if(!core_item(a)) continue;
    if(set_n >= RA_PATCH_MAX) {
      debugf("RA: more than %u achievements, the rest is ignored", (unsigned)RA_PATCH_MAX);
      break;
    }
    // rcheevos parses the condition string and checks it from the next frame on,
    // it fires only after the condition was false once. NULL, 0 are the Lua
    // arguments, unused. A string it cannot parse, or no memory for it, and the
    // achievement is not activated and does not enter the table.
    int rv = rc_runtime_activate_achievement(rt, a->id, a->definition, NULL, 0);
    if(rv != RC_OK) {
      debugf("RA: condition %u (%s) rejected, code %d, it will never fire",
             (unsigned)a->id, a->title ? a->title : "", rv);
      rejected++;
      continue;
    }
    // keep id and title, snprintf cuts a long title to the table
    set[set_n].id = a->id;
    snprintf(set[set_n].title, sizeof(set[set_n].title), "%s", a->title ? a->title : "");
    set_n++;
  }

  // leaderboards come with the set but are not evaluated yet, the log counts them

  debugf("RA: set for '%s': %u achievements active, %u rejected, %u leaderboards ignored",
         r->title ? r->title : "?", set_n, rejected, (unsigned)r->num_leaderboards);
  return (int)set_n;
}

int ra_patch_load(rc_runtime_t *rt) {
  rc_api_fetch_game_data_response_t r;
  int n = -1;

  // read, parse, activate. Each step logs its own failure. The parsed response
  // is freed in every case, rcheevos has copied what it needs.
  if(!card_read()) return -1;
  memset(&r, 0, sizeof(r));
  if(parse_body(&r) == 0) n = activate_set(rt, &r);
  rc_api_destroy_fetch_game_data_response(&r);
  return n;
}
