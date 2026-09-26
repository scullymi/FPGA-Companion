/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.c
 *  @brief The achievement set of the current game.
 *
 *  The conditions belong to RetroAchievements and are not compiled into the
 *  firmware. The set lies on the card as RA_PATCH_FILE, the server's reply to
 *  r=patch, and rcheevos' own parser reads it (rc_api_runtime.c). Nothing here
 *  interprets the JSON. Titles are copied into a small table, the parsed
 *  conditions live inside rcheevos. The RA task fetches the set from the
 *  server once per session and writes it to the card when it changed, so it
 *  is there offline too.
 *
 *  Two tasks share the work: the RA task reads and parses, so the game loop
 *  never waits for the card, and hands the parsed set over a queue of one to
 *  com_task, which activates it and alone changes rcheevos and the table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "rc_api_runtime.h"
#include "rcheevos/src/rhash/md5.h"
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
#define RA_PATCH_TMP        RA_PATCH_FILE ".new"  /**< a new set while it is written, then it takes the place of the old */
/** "Warning: Unknown Emulator", which the server adds for clients it does not
   know. Not an achievement of the game. */
#define RA_PATCH_WARNING_ID 101000001u

// the set as it was read from the card or received from the server, for
// rcheevos' parser. Static, 40 KB would not fit on a task's stack. RA task only.
static char     body[RA_PATCH_BODY_MAX];
static unsigned body_len;

// what the set on the card amounts to, to tell whether the server's is a new
// one: id, title and condition of every core achievement, the rest of the
// reply may change from request to request
static unsigned char card_fp[16];
static bool          card_fp_valid;

// one parsed set on its way from the RA task to com_task. rcheevos copies what
// it needs out of the file, so a parsed set does not depend on body[].
static QueueHandle_t handover;

// the achievements that are active: the title for the log, the 1-based position
// for the FPGA back channel and the RA task. rcheevos only reports an id when
// one fires. Written by com_task only.
/** One achievement of the active set. */
typedef struct {
  unsigned id;                        /**< achievement id on the server */
  char     title[RA_PATCH_TITLE_MAX];  /**< its title, cut to fit */
} entry_t;
static entry_t  set[RA_PATCH_MAX];
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

static int hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Undoes the chunked transfer framing that lwIP's HTTP client passes through:
   each chunk is a hex length, CR LF, the data, CR LF, and a length of 0 ends it.
   The data moves down in place. A body that starts with '{' is plain already.
   false when it is neither. */
static bool strip_chunks(void) {
  char *in = body, *out = body, *end = body + body_len;

  if(body_len == 0) return false;
  if(body[0] == '{') return true;
  for(;;) {
    unsigned long n = 0;
    char *p = in;
    while(p < end && hexval(*p) >= 0) { n = n * 16 + (unsigned long)hexval(*p); p++; }
    if(p == in) return false;                       // no length line: not chunked
    while(p < end && *p != '\n') p++;               // chunk extensions and the CR
    if(p >= end) return false;
    p++;                                            // the LF
    if(n == 0) break;                               // the last chunk
    if((unsigned long)(end - p) < n) return false;  // cut short
    memmove(out, p, n);
    out += n;
    in = p + n;
    if(in < end && *in == '\r') in++;
    if(in < end && *in == '\n') in++;
  }
  *out = 0;
  body_len = (unsigned)(out - body);
  return true;
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
  // a second file left behind by card_write(). Alone, it is all there is, so it
  // becomes the set and the read below judges it, unless it is empty. Next to
  // the old set, the old one stays: it was whole when the second was started.
  // (A power cut inside FatFs' rename itself could leave both names on one
  // cluster chain, that case is not told apart here.)
  { FILINFO fi;
    bool    tmp  = f_stat(RA_PATCH_TMP, &fi) == FR_OK;
    FSIZE_t size = tmp ? fi.fsize : 0;
    bool    old  = f_stat(RA_PATCH_FILE, &fi) == FR_OK;
    if(tmp && !old && size) f_rename(RA_PATCH_TMP, RA_PATCH_FILE);
    else if(tmp)            f_unlink(RA_PATCH_TMP);
  }
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
    if(!strip_chunks()) {       // a file put on the card from outside may still carry the framing
      debugf("RA: set on the card is not a set, no achievements");
      return false;
    }
    return true;
  }
  return false;
}

/* Writes the set to the card: to a second file first, which then takes the
   place of the old one, so a power cut leaves the old set and not half of the
   new one. card_read() puts a file back that was left under the second name. */
static bool card_write(void) {
  FIL f;
  UINT put = 0;
  FRESULT r;

  sdc_lock();
  r = f_open(&f, RA_PATCH_TMP, FA_WRITE | FA_CREATE_ALWAYS);
  if(r == FR_OK) {
    r = f_write(&f, body, body_len, &put);
    FRESULT c = f_close(&f);      // f_close writes the last sector, its result counts
    if(r == FR_OK) r = c;
    if(r == FR_OK && put != body_len) r = FR_DISK_ERR;
  }
  if(r == FR_OK) {
    f_unlink(RA_PATCH_FILE);      // may not exist yet
    r = f_rename(RA_PATCH_TMP, RA_PATCH_FILE);
  }
  sdc_unlock();
  if(r != FR_OK) debugf("RA: %s not written (error %d), the set is not there offline", RA_PATCH_FILE, (int)r);
  return r == FR_OK;
}

/* Only the official achievements count. The set also holds unofficial ones
   (category 5, not finished or under review) and the server's pseudo
   achievement RA_PATCH_WARNING_ID, both are left out. */
static bool core_item(const rc_api_achievement_definition_t *a) {
  return a->category == RC_ACHIEVEMENT_CATEGORY_CORE && a->id != RA_PATCH_WARNING_ID;
}

/* A fingerprint of a parsed set: md5 per core achievement over id, title and
   condition, all of them folded together with xor, so the order in the reply
   does not matter. Two sets with the same fingerprint play the same. */
static void fingerprint(const rc_api_fetch_game_data_response_t *r, unsigned char *sum) {
  unsigned i, k;
  memset(sum, 0, 16);
  for(i = 0; i < r->num_achievements; i++) {
    const rc_api_achievement_definition_t *a = &r->achievements[i];
    md5_state_t md5;
    unsigned char one[16];
    if(!core_item(a)) continue;
    md5_init(&md5);
    md5_append(&md5, (const md5_byte_t *)&a->id, sizeof(a->id));
    if(a->title)      md5_append(&md5, (const md5_byte_t *)a->title, (int)strlen(a->title) + 1);
    if(a->definition) md5_append(&md5, (const md5_byte_t *)a->definition, (int)strlen(a->definition));
    md5_finish(&md5, one);
    for(k = 0; k < 16; k++) sum[k] ^= one[k];
  }
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
  // unusable: not JSON or broken JSON, a required field missing, or a reply
  // the server marked as failed. The server's text, else rcheevos' own.
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
  entry_t  next[RA_PATCH_MAX];
  unsigned i, j, n = 0, rejected = 0;

  // every achievement of the set once: skip what does not count, stop when the
  // table is full, and hand the condition string of the others to rcheevos.
  // Stopping keeps rcheevos and the table in step: an achievement active without
  // a table entry would fire without title and position.
  for(i = 0; i < r->num_achievements; i++) {
    const rc_api_achievement_definition_t *a = &r->achievements[i];
    if(!core_item(a)) continue;
    if(n >= RA_PATCH_MAX) {
      debugf("RA: more than %u achievements, the rest is ignored", (unsigned)RA_PATCH_MAX);
      break;
    }
    // rcheevos parses the condition string and checks it from the next frame on,
    // it fires only after the condition was false once. NULL, 0 are the Lua
    // arguments, unused. A string it cannot parse is not activated and does not
    // enter the table. One it knows already with the same condition keeps running.
    int rv = rc_runtime_activate_achievement(rt, a->id, a->definition, NULL, 0);
    if(rv != RC_OK) {
      debugf("RA: condition %u (%s) rejected, code %d, it will never fire",
             (unsigned)a->id, a->title ? a->title : "", rv);
      rejected++;
      continue;
    }
    // keep id and title, snprintf cuts a long title to the table
    next[n].id = a->id;
    snprintf(next[n].title, sizeof(next[n].title), "%s", a->title ? a->title : "");
    n++;
  }

  // what the previous set had and this one does not carry leaves rcheevos too,
  // so nothing fires without an entry in the table
  for(i = 0; i < set_n; i++) {
    for(j = 0; j < n && next[j].id != set[i].id; j++) ;
    if(j == n) rc_runtime_deactivate_achievement(rt, set[i].id);
  }

  // the table goes out in one piece: other tasks read it without a lock, and on
  // this single core a change with interrupts off cannot be seen half done
  taskENTER_CRITICAL();
  memcpy(set, next, n * sizeof(set[0]));
  set_n = n;
  taskEXIT_CRITICAL();

  // leaderboards come with the set but are not evaluated yet, the log counts them
  debugf("RA: set for '%s': %u achievements active, %u rejected, %u leaderboards ignored",
         r->title ? r->title : "?", n, rejected, (unsigned)r->num_leaderboards);
  return (int)n;
}

bool ra_patch_init(void) {
  if(!handover) handover = xQueueCreate(1, sizeof(rc_api_fetch_game_data_response_t *));
  return handover != NULL;
}

static void discard(rc_api_fetch_game_data_response_t *r) {
  rc_api_destroy_fetch_game_data_response(r);
  free(r);
}

/* Parses body into a set on the heap, which lives there until com_task has
   activated it. n gets the number of core achievements. NULL when body is no
   usable set, parse_body() says why. */
static rc_api_fetch_game_data_response_t *parse_new(unsigned *n) {
  rc_api_fetch_game_data_response_t *r = malloc(sizeof(*r));
  unsigned i;
  if(!r) return NULL;      // a guard only, pico_malloc stops the firmware when the heap is out
  memset(r, 0, sizeof(*r));
  if(parse_body(r) != 0) {
    discard(r);
    return NULL;
  }
  for(*n = 0, i = 0; i < r->num_achievements; i++) if(core_item(&r->achievements[i])) (*n)++;
  return r;
}

/* Hands a set to com_task. One it has not taken yet gives way, the newer one counts. */
static void hand_over(rc_api_fetch_game_data_response_t *r) {
  rc_api_fetch_game_data_response_t *old;
  if(xQueueReceive(handover, &old, 0) == pdTRUE) discard(old);
  xQueueSend(handover, &r, 0);
}

int ra_patch_read_card(void) {
  rc_api_fetch_game_data_response_t *r;
  unsigned n;

  if(!handover || !card_read()) return -1;
  if(!(r = parse_new(&n))) return -1;
  fingerprint(r, card_fp);
  card_fp_valid = true;
  hand_over(r);
  return (int)n;
}

char *ra_patch_body(unsigned *cap) {
  *cap = sizeof(body);
  return body;
}

int ra_patch_from_server(unsigned len) {
  rc_api_fetch_game_data_response_t *r;
  unsigned char sum[16];
  unsigned n;

  body_len = len;
  body[len] = 0;
  if(!handover || !strip_chunks()) {
    debugf("RA: reply from the server is not a set");
    return -1;
  }
  if(!(r = parse_new(&n))) return -1;
  // the same achievements as on the card: nothing to write, nothing to hand
  // over. The set that runs came from the card at start.
  fingerprint(r, sum);
  if(card_fp_valid && memcmp(sum, card_fp, sizeof(sum)) == 0) {
    debugf("RA: set unchanged on the server");
    discard(r);
    return 0;
  }
  // the card first, so a power cut after this leaves the new set there too
  if(card_write()) {
    memcpy(card_fp, sum, sizeof(card_fp));
    card_fp_valid = true;
    debugf("RA: set from the server: %u core achievements, kept on the card", n);
  } else
    debugf("RA: set from the server: %u core achievements, only in memory", n);
  hand_over(r);
  return 1;
}

void ra_patch_apply_pending(rc_runtime_t *rt) {
  rc_api_fetch_game_data_response_t *r;
  if(!handover || xQueueReceive(handover, &r, 0) != pdTRUE) return;
  activate_set(rt, r);
  discard(r);               // rcheevos has copied the conditions
}
