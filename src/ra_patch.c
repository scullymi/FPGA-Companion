/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.c
 *  @brief The achievement set of the current game.
 *
 *  The conditions belong to RetroAchievements and are not compiled into the
 *  firmware. The set lies on the card in the game's folder, /sd/ra/ plus the
 *  game's id, with the file names of ra_games.h, as the server's reply to
 *  r=patch, and rcheevos' own parser reads it (rc_api_runtime.c). Nothing here
 *  interprets the JSON. Titles are copied into a small table, the parsed
 *  conditions live inside rcheevos. The RA task fetches the set from the server
 *  once per session and writes it to the card when it changed, so it is there
 *  offline too.
 *
 *  Which game: decided once per boot by ra_patch_settle() from the board id of
 *  the core (RAM mirror header), the SHA-256 of the ROM image and the image's
 *  name, with the rules of ra_games_select(). The card set is read right there,
 *  in com_task, before the RA task exists.
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
#include "rc_runtime_types.h"   // rc_trigger_t and its states, for the challenge indicator
#include "rcheevos/src/rhash/md5.h"
#include "rcheevos/src/rapi/rc_api_common.h"   // rc_json_*: one field of the set text, decoded as rc_api does
#include "rc_util.h"                             // rc_buffer_t for that one field
#include "mbedtls/sha256.h"
#include <ff.h>
#include "debug.h"
#include "sdc.h"
#include "ra_net.h"
#include "ra_patch.h"
#include "ra_slim.h"
#include "ra_task.h"
#include "ra_mac.h"

uint32_t getFreeHeap(void);   // mcu_hw.c, or the weak stand-in in ra_net.c: the SDK heap left

/* The identity of this boot's game, written once by ra_patch_settle() in com_task
   and read by every task through the accessors. The game table (ra_games.c) names
   the games, this only says which one runs. */
static const ra_game_t *game;                          // the table entry, NULL for a fallback identity or no game
static unsigned         game_id;                       // its id, 0 for a fallback identity, a wrong board or no game
static char             game_hash[RA_GAMES_HASH_LEN + 1];   // what the server is asked with, "" when there is no game
static volatile unsigned resolved_id;                  // fallback identity: the id the server gave, 0 until then
static const char      *rom_label = "";                // what the ROM file is, for the Version dialog
static volatile unsigned char board;                   // header byte 12, 0 until a valid header was read
static bool             settled;                       // ra_patch_settle() ran
static bool             table_wrong;                   // the server's id differs from the table's: RA_HC_BLOCK_GAME stays
static bool             foreign_rom;                   // the ROM in the core is another game's than the boot's, see ra_patch_settle()
static const ra_game_t *volatile restart_to;           // the game a restart starts, see ra_patch_restart_to()
static volatile bool   restart_committed;             // restart_step() writes the marker now: picks no longer change the target
static unsigned        pick_seq;                      // picks of a game of another board, counted
static unsigned        stream_seq;                    // pick_seq when the ROM that streams now started
static bool             set_off;                       // ra_patch_apply_pending() took the set out of rcheevos for that
static volatile bool    set_again;                     // com_task asks the RA task to read the card set once more

/* What the ROM stream leaves behind for settle. rom_start and rom_gone may run in
   menu_task, rom_data and rom_end in com_task, settle in com_task. The stream orders
   start, end and settle on the normal paths; the 15 s timeout settle of the main
   loop is not ordered by it, so rom_start and rom_gone publish their stores with
   interrupts off and settle takes its inputs the same way, whole or not at all. */
static mbedtls_sha256_context rom_sha;                 // the ROM image while it streams
static bool                   rom_hashing;
static volatile bool          rom_pending;             // rom_start ran, rom_end or rom_gone did not yet: the main loop holds its settle back
static volatile TickType_t    rom_started;             // when the stream that runs now began, see ra_patch_rom_streaming()
static unsigned char          rom_sum[32];             // its SHA-256 once the stream ended
static bool                   rom_sum_valid;
static volatile bool          rom_bad;                 // the ROM in the core is damaged, or none came for an old file: no game
static char                   rom_name_hash[RA_GAMES_HASH_LEN + 1];   // md5 of the image's name (arcade rule), "" when nothing streamed

#define RA_PATCH_BODY_MAX   65536        /**< a whole set, e.g. Galaga has about 15.7 KB, the Perfect Pac subset of Pac-Man 53.6 KB */
#define RA_PATCH_MAC_LABEL  "g20k-s1"    /**< what the set's tag is made over, keeps it apart from other tags */

// the set's files in the game's folder, "/sd/ra/<id>/...", filled by set_paths()
// once the id is known
static char set_path[RA_GAMES_PATH_MAX];   // the set
static char set_tmp[RA_GAMES_PATH_MAX];    // a new set while it is written
static char set_mac[RA_GAMES_PATH_MAX];    // its tag
/** The first id of the server's warnings, e.g. "Warning: Unknown Emulator", which
   it adds as an achievement for a client it has not fully approved for hardcore.
   Not an achievement of the game, rc_client treats every id from here on as one. */
#define RA_PATCH_WARNING_ID 101000001u

// the set as it was read from the card or received from the server, for
// rcheevos' parser. Static, 40 KB would not fit on a task's stack. com_task uses
// it in settle (read_card, move_file) before the RA task is started, from then on
// RA task only.
static char     body[RA_PATCH_BODY_MAX];
static unsigned body_len;

// what the set on the card amounts to, to tell whether the server's is a new
// one: id, title and condition of every core achievement, the rich presence
// script, the leaderboards and the server's warning. The rest of the reply may
// change from request to request.
static unsigned char card_fp[16];
static bool          card_fp_valid;
// card_fp is the fingerprint of the set that runs, read from the card or written
// there; card_fp_valid adds that the card's copy carries this Pico's tag
static bool          card_fp_runs;

// the title of the server's warning in the set parsed last, without "Warning: ",
// "" when it had none. Written by com_task (card) and the RA task (server), read
// by any task, always with interrupts off.
static char warning[RA_PATCH_TITLE_MAX];

// the core was reset, rcheevos starts over before the next frame. Set by any
// task, cleared by com_task.
static volatile bool reset_due;

// the set file read from the card carried a tag that checks out: it is the set
// this Pico received from the server, not one edited on a computer
static bool card_verified;

/* The set's tag is made over the game id (4 bytes, little endian) and the file as it
   lies on the card, as two parts: a copy of the set with the id in front would be a
   second buffer of its size. */
static void set_tag_id(unsigned char id4[4]) {
  unsigned id = ra_game_id();
  id4[0] = (unsigned char)id; id4[1] = (unsigned char)(id >> 8); id4[2] = (unsigned char)(id >> 16); id4[3] = (unsigned char)(id >> 24);
}
static bool set_tag(const char *data, unsigned len, char *hex) {
  unsigned char id4[4];
  set_tag_id(id4);
  return ra_mac_tag2(RA_PATCH_MAC_LABEL, id4, sizeof(id4), data, len, hex);
}
static bool set_check(const char *data, unsigned len, const char *hex) {
  unsigned char id4[4];
  set_tag_id(id4);
  return ra_mac_check2(RA_PATCH_MAC_LABEL, id4, sizeof(id4), data, len, hex);
}

// one parsed set on its way from the RA task to com_task. rcheevos copies what
// it needs out of the file, so a parsed set does not depend on body[].
static QueueHandle_t handover;

// the achievements that are active: the title for the log, the 1-based position
// for the FPGA back channel and the RA task. rcheevos only reports an id when
// one fires. Written by com_task only.
/** One achievement of the active set. */
typedef struct {
  unsigned      id;                        /**< achievement id on the server */
  unsigned      points;                    /**< its points */
  const char   *title;                     /**< its title, cut to fit, in text_pool */
  const char   *desc;                      /**< its description, in text_pool */
  unsigned char md5[16];                   /**< md5 of its condition, an unchanged one keeps running */
} entry_t;
static entry_t  set[RA_PATCH_MAX];
static unsigned set_n;
// titles and descriptions of the active set, one block sized to the set: Galaga's 17
// take about 2 KB, fixed fields in the table would take 36 KB for RA_PATCH_MAX
static char    *text_pool;

/** What rcheevos says about an achievement of the table, same index. */
typedef struct {
  char progress[RA_PATCH_PROGRESS_MAX];     /**< measured progress, "" when none */
  bool primed;                              /**< challenge on */
} live_t;
static live_t live[RA_PATCH_MAX];           // written by com_task, read by the menu

/** One leaderboard of the active set. */
typedef struct {
  ra_patch_lboard_t info;                   /**< what other tasks may read */
  unsigned char     md5[16];                /**< md5 of its definition, an unchanged one keeps running */
} lb_entry_t;
static lb_entry_t lb_set[RA_PATCH_LB_MAX];
static unsigned   lb_n;

// achievements, leaderboards and the rich presence script that found no memory
// while the parsed set was still on the heap: once it is freed they are activated from
// the set's text in body[], see activate_deferred(). A large one needs as much
// memory as the rest of the set, the parsed set as much again. com_task only,
// handed_fp is written by whoever hands a set over, before it does.
static unsigned      deferred_ach[RA_PATCH_MAX];   // rows of the table
static unsigned      deferred_ach_n;
static lb_entry_t    deferred_lb[RA_PATCH_LB_MAX];
static unsigned      deferred_lb_n;
static unsigned      oom_off;        // parts of the set that stay off, for lack of memory or room in the table: hardcore is blocked
static bool          deferred_rp;
static unsigned char deferred_rp_md5[16];
static unsigned char handed_fp[16];   // fingerprint of body[] the handed-over set was parsed from

// the rich presence script that runs, by its md5. com_task only.
static unsigned char rp_md5[16];
static bool          rp_on;

const char   *ra_game_hash(void)      { return game_hash; }
unsigned      ra_game_id(void)        { return game_id ? game_id : resolved_id; }
const char   *ra_game_title(void)     { return game ? game->title : NULL; }
unsigned char ra_game_board(void)     { return board; }
const char   *ra_game_rom_label(void) { return rom_label; }
// the switches of the game that is played: a table entry on the wrong board is
// not played, so its switches do not matter either
const ra_dip_t *ra_game_dips(unsigned *n) {
  *n = game && game_id ? game->dip_n : 0;
  return *n ? game->dips : NULL;
}

/* The set's three paths for the current id. False when there is no id yet: a
   fallback identity has no folder until the server resolved it. */
static bool set_paths(void) {
  unsigned id = ra_game_id();
  return ra_games_path(id, RA_GAMES_SET_FILE, set_path, sizeof(set_path)) &&
         ra_games_path(id, RA_GAMES_SET_TMP,  set_tmp,  sizeof(set_tmp)) &&
         ra_games_path(id, RA_GAMES_SET_MAC,  set_mac,  sizeof(set_mac));
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

bool ra_patch_lboard(unsigned id, ra_patch_lboard_t *out) {
  bool ok = false;
  unsigned i;
  taskENTER_CRITICAL();
  for(i = 0; i < lb_n; i++)
    if(lb_set[i].info.id == id) { *out = lb_set[i].info; ok = true; break; }
  taskEXIT_CRITICAL();
  return ok;
}

bool ra_patch_warning(char *out, size_t size) {
  bool on;
  taskENTER_CRITICAL();
  on = warning[0] != 0;
  if(out && size) snprintf(out, size, "%s", warning);
  taskEXIT_CRITICAL();
  return on;
}

bool ra_patch_item(unsigned i, ra_patch_item_t *out) {
  bool ok = false;
  // one copy with interrupts off, so an entry is never seen half replaced
  taskENTER_CRITICAL();
  if(i < set_n) {
    out->id     = set[i].id;
    out->points = set[i].points;
    snprintf(out->title, sizeof(out->title), "%s", set[i].title ? set[i].title : "");
    snprintf(out->desc, sizeof(out->desc), "%s", set[i].desc ? set[i].desc : "");
    memcpy(out->progress, live[i].progress, sizeof(out->progress));
    out->primed = live[i].primed;
    ok = true;
  }
  taskEXIT_CRITICAL();
  return ok;
}

void ra_patch_format_progress(const rc_runtime_t *rt, unsigned id, char *buf, size_t size) {
  const rc_trigger_t *t = rc_runtime_get_achievement(rt, id);
  uint32_t value;
  buf[0] = 0;
  // nothing measured, or not active (disabled, inactive, fired): no progress, as
  // rcheevos' own formatter decides it
  if(!t || !t->measured_target || t->state == RC_TRIGGER_STATE_DISABLED ||
     t->state == RC_TRIGGER_STATE_INACTIVE || t->state == RC_TRIGGER_STATE_TRIGGERED)
    return;
  // unknown after a reset, or 0: nothing to show yet (rcheevos would print "0/50")
  value = t->measured_value;
  if(value == 0xFFFFFFFFu || value == 0) return;
  if(value > t->measured_target) value = t->measured_target;
  if(t->measured_as_percent) {
    uint32_t percent = (uint32_t)(((unsigned long long)value * 100) / t->measured_target);
    if(percent) snprintf(buf, size, "%u%%", (unsigned)percent);
  } else
    snprintf(buf, size, "%u/%u", (unsigned)value, (unsigned)t->measured_target);
}

void ra_patch_update_progress(const rc_runtime_t *rt) {
  unsigned i;
  for(i = 0; i < set_n; i++) {
    live_t one;
    // the progress, "12/50" or a percentage. Primed: only the trigger condition is
    // missing, the challenge indicator of RetroAchievements.
    rc_trigger_t *t = rc_runtime_get_achievement(rt, set[i].id);
    ra_patch_format_progress(rt, set[i].id, one.progress, sizeof(one.progress));
    one.primed = t && t->state == RC_TRIGGER_STATE_PRIMED;
    taskENTER_CRITICAL();
    live[i] = one;
    taskEXIT_CRITICAL();
  }
}

/* The set without the chunked framing, see ra_net_dechunk(). */
static bool strip_chunks(void) { return ra_net_dechunk(body, &body_len); }

/* A set from the card goes through ra_slim like one from the server, in place: framing
   off and the unused fields out. So a file kept before ra_slim compares equal to the
   server's set, and an unchanged set is not parsed a second time. */
static bool slim_body(void) {
  ra_slim_t s;
  unsigned n = 0;
  ra_slim_init(&s);
  if(!ra_slim_feed(&s, body, body_len, body, sizeof(body), &n) || !ra_slim_whole(&s)) return false;
  body_len = n;
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

  if(!set_paths()) return false;   // no id: no folder to read from
  // the card is shared with other tasks, only the file access runs under the lock
  sdc_lock();
  // a second file left behind by card_write(). Alone, it is all there is, so it
  // becomes the set and the read below judges it, unless it is empty. Next to
  // the old set, the old one stays: it was whole when the second was started.
  // (A power cut inside FatFs' rename itself could leave both names on one
  // cluster chain, that case is not told apart here.)
  { FILINFO fi;
    bool    tmp  = f_stat(set_tmp, &fi) == FR_OK;
    FSIZE_t size = tmp ? fi.fsize : 0;
    bool    old  = f_stat(set_path, &fi) == FR_OK;
    if(tmp && !old && size) f_rename(set_tmp, set_path);
    else if(tmp)            f_unlink(set_tmp);
  }
  r = f_open(&f, set_path, FA_READ);
  if(r == FR_OK) {
    size = f_size(&f);
    // read only a set that fits whole, one byte stays free for the final NUL
    if(size && size < sizeof(body)) r = f_read(&f, body, (UINT)size, &got);
    f_close(&f);
  }
  // its tag, one line "g20k-s1 <64 hex>"
  char mac_line[16 + RA_MAC_HEX];
  bool mac_read = false;
  { FIL m;
    UINT mgot = 0;
    if(f_open(&m, set_mac, FA_READ) == FR_OK) {
      mac_read = f_read(&m, mac_line, sizeof(mac_line) - 1, &mgot) == FR_OK;
      mac_line[mgot] = 0;
      f_close(&m);
    }
  }
  sdc_unlock();

  // one log line per failure: no file, empty, too large, and last any other open
  // or read error (FatFs returns the whole file with FR_OK, got != size only guards)
  if(r == FR_NO_FILE || r == FR_NO_PATH)
    debugf("RA: no set on the card (%s), no achievements", set_path);
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
    // the tag over the file as it lies on the card, before anything is changed in body
    card_verified = false;
    if(mac_read && !strncmp(mac_line, RA_PATCH_MAC_LABEL " ", strlen(RA_PATCH_MAC_LABEL) + 1)) {
      char *hex = mac_line + strlen(RA_PATCH_MAC_LABEL) + 1;
      hex[strcspn(hex, "\r\n")] = 0;
      card_verified = set_check(body, body_len, hex);
    }
    if(!slim_body()) {          // a file put on the card from outside may still carry the framing
      debugf("RA: set on the card is not a set, no achievements");
      return false;
    }
    return true;
  }
  return false;
}

/* Writes the set to the card: to a second file first, which then takes the
   place of the old one, so a power cut leaves the old set and not half of the
   new one. card_read() puts a file back that was left under the second name. The
   tag goes first away and last in: a power cut in between leaves a set without a
   tag, which only costs hardcore until the server is reached. */
static bool card_write(void) {
  FIL f;
  UINT put = 0;
  FRESULT r;
  char hex[RA_MAC_HEX + 1];
  bool tagged = set_tag(body, body_len, hex);

  if(!set_paths()) return false;   // no id: nowhere to write to
  sdc_lock();
  // the game's folder, cheap when it exists: a fallback identity's folder is made
  // when the server resolved the id, this defends against a failed attempt then
  r = ra_games_mkdir(ra_game_id());
  if(r == FR_OK) f_unlink(set_mac);   // may not exist
  if(r == FR_OK) r = f_open(&f, set_tmp, FA_WRITE | FA_CREATE_ALWAYS);
  if(r == FR_OK) {
    r = f_write(&f, body, body_len, &put);
    FRESULT c = f_close(&f);      // f_close writes the last sector, its result counts
    if(r == FR_OK) r = c;
    if(r == FR_OK && put != body_len) r = FR_DISK_ERR;
  }
  if(r == FR_OK) {
    f_unlink(set_path);           // may not exist yet
    r = f_rename(set_tmp, set_path);
  }
  // the tag, when there is a device key to make one
  if(r == FR_OK && tagged && f_open(&f, set_mac, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    bool ok = f_printf(&f, "%s %s\n", RA_PATCH_MAC_LABEL, hex) > 0;
    if(f_close(&f) != FR_OK || !ok) f_unlink(set_mac);
  }
  sdc_unlock();
  if(r != FR_OK) debugf("RA: %s not written (error %d), the set is not there offline", set_path, (int)r);
  return r == FR_OK;
}

/* Only the official achievements count. The set also holds unofficial ones
   (category 5, not finished or under review) and the server's warning from
   RA_PATCH_WARNING_ID on, both are left out. */
static bool core_item(const rc_api_achievement_definition_t *a) {
  return a->category == RC_ACHIEVEMENT_CATEGORY_CORE && a->id < RA_PATCH_WARNING_ID;
}

/* Notes the server's warning about this client from a parsed set, see
   ra_patch_warning(). The server marks it as unlocked in casual at the session
   start, so that it shows in hardcore only, and com_task shows it that way. */
static void note_warning(const rc_api_fetch_game_data_response_t *r) {
  const rc_api_achievement_definition_t *w = NULL;
  const char *t = "";
  bool changed;
  unsigned i;
  for(i = 0; i < r->num_achievements && !w; i++)
    if(r->achievements[i].id >= RA_PATCH_WARNING_ID) w = &r->achievements[i];
  if(w) {
    t = w->title ? w->title : "";
    if(!strncmp(t, "Warning: ", 9)) t += 9;
    if(!*t) t = "Warning";            // one without a title still counts
  }
  taskENTER_CRITICAL();
  changed = strncmp(warning, t, sizeof(warning) - 1) != 0;
  snprintf(warning, sizeof(warning), "%s", t);
  taskEXIT_CRITICAL();
  if(changed && w)
    debugf("RA: server warning '%s': %s", t, w->description ? w->description : "");
  else if(changed)
    debugf("RA: no server warning any more");
}

/* A fingerprint of a set as the server sent it, without parsing it: an md5 over
   the reply, with the values left out of the keys that change from request to
   request: Rarity and RarityHardcore (unlock statistics), Modified and Created
   (the server's warning carries the time of the request there). The same set
   gives the same fingerprint, and anything that plays or shows differently,
   conditions, titles, leaderboards, the rich presence script or the warning,
   gives another. It needs no memory: rcheevos' parse of a large set takes about
   as much again as the set itself, more than is left once a large set runs. */
static void fingerprint(const char *s, unsigned len, unsigned char *sum) {
  static const char *const skip[] = { "\"Rarity\":", "\"RarityHardcore\":", "\"Modified\":", "\"Created\":" };
  md5_state_t md5;
  unsigned i = 0, from = 0, k, n;
  md5_init(&md5);
  while(i < len) {
    // a key starts at an unescaped quote; one of the four: hash up to and with
    // the key, then step over its value, a number or null
    if(s[i] == '"' && (i == 0 || s[i - 1] != '\\')) {
      for(k = 0; k < sizeof(skip) / sizeof(skip[0]); k++) {
        n = (unsigned)strlen(skip[k]);
        if(len - i >= n && !memcmp(s + i, skip[k], n)) break;
      }
      if(k < sizeof(skip) / sizeof(skip[0])) {
        unsigned j = i + n;
        md5_append(&md5, (const md5_byte_t *)s + from, (int)(j - from));
        if(len - j >= 4 && !memcmp(s + j, "null", 4)) j += 4;
        else while(j < len && s[j] && strchr("0123456789.-+eE", s[j])) j++;
        from = i = j;
        continue;
      }
    }
    i++;
  }
  md5_append(&md5, (const md5_byte_t *)s + from, (int)(len - from));
  md5_finish(&md5, sum);
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

/* md5 of a string, for the conditions and the rich presence script. */
static void md5_of(const char *s, unsigned char *sum) {
  md5_state_t md5;
  md5_init(&md5);
  md5_append(&md5, (const md5_byte_t *)s, (int)strlen(s));
  md5_finish(&md5, sum);
}

/* rcheevos frees the old script before it allocates the new one. When the new
   one's buffer finds no memory, it leaves an entry with the md5 set but neither
   buffer nor script pointer set. The next call with the same script takes that
   entry for unchanged and resets through the unset pointer, and every frame reads
   it, so the entry goes here. */
static void drop_half_richpresence(rc_runtime_t *rt) {
  if(rt->richpresence && !rt->richpresence->buffer) {
    free(rt->richpresence);
    rt->richpresence = NULL;
  }
}

/* The rich presence script of a set. rcheevos resets a script it is given again
   and keeps the old one when given an empty one, so an unchanged script is left
   alone, and an empty one is freed here the way rc_runtime_destroy() does it.
   Returns the result of the activation, RC_OK when there was nothing to do. */
static int activate_richpresence(rc_runtime_t *rt, const char *script) {
  unsigned char sum[16];
  if(!script || !*script) {
    if(rt->richpresence) {
      free(rt->richpresence->buffer);
      free(rt->richpresence);
      rt->richpresence = NULL;
    }
    rp_on = false;
    return RC_OK;
  }
  md5_of(script, sum);
  if(rp_on && memcmp(sum, rp_md5, sizeof(sum)) == 0) return RC_OK;
  int rv = rc_runtime_activate_richpresence(rt, script, NULL, 0);
  if(rv == RC_OUT_OF_MEMORY) drop_half_richpresence(rt);
  if(rv == RC_OK) {
    memcpy(rp_md5, sum, sizeof(rp_md5));
    rp_on = true;
  } else {
    if(!rt->richpresence || !rt->richpresence->richpresence)
      rp_on = false;        // a parse error keeps the old script, running out of memory frees it
    if(rv == RC_OUT_OF_MEMORY) {
      deferred_rp = true;   // again once the parsed set is freed
      memcpy(deferred_rp_md5, sum, sizeof(deferred_rp_md5));
    }
  }
  return rv;
}

/* Activates the set's leaderboards, the same way as the achievements: one whose
   definition did not change keeps running, rcheevos would restart an attempt when
   given the same definition again. Hidden ones run too, hidden only concerns lists.
   RetroAchievements asks that leaderboards work and cannot be switched off in
   hardcore. Returns the number that run. */
static unsigned activate_lboards(rc_runtime_t *rt, const rc_api_fetch_game_data_response_t *r,
                                 unsigned *kept, unsigned *rejected) {
  static lb_entry_t next[RA_PATCH_LB_MAX];   // com_task only
  unsigned i, j, n = 0;
  *kept = *rejected = 0;
  for(i = 0; i < r->num_leaderboards; i++) {
    const rc_api_leaderboard_definition_t *l = &r->leaderboards[i];
    unsigned char sum[16];
    if(n >= RA_PATCH_LB_MAX) {
      debugf("RA: more than %u leaderboards, the rest is ignored", (unsigned)RA_PATCH_LB_MAX);
      break;
    }
    md5_of(l->definition ? l->definition : "", sum);
    for(j = 0; j < lb_n && !(lb_set[j].info.id == l->id && !memcmp(lb_set[j].md5, sum, sizeof(sum))); j++) ;
    if(j < lb_n)
      (*kept)++;
    else {
      int rv = rc_runtime_activate_lboard(rt, l->id, l->definition, NULL, 0);
      if(rv == RC_OUT_OF_MEMORY && deferred_lb_n < RA_PATCH_LB_MAX) {
        // again once the parsed set is freed, with what the table needs kept here
        lb_entry_t *d = &deferred_lb[deferred_lb_n++];
        d->info.id              = l->id;
        d->info.format          = l->format;
        d->info.lower_is_better = l->lower_is_better != 0;
        snprintf(d->info.title, sizeof(d->info.title), "%s", l->title ? l->title : "");
        memcpy(d->md5, sum, sizeof(sum));
        continue;
      }
      if(rv == RC_OUT_OF_MEMORY) oom_off++;
      if(rv != RC_OK) {
        debugf("RA: leaderboard %u (%s) rejected, code %d", (unsigned)l->id, l->title ? l->title : "", rv);
        (*rejected)++;
        continue;
      }
    }
    next[n].info.id              = l->id;
    next[n].info.format          = l->format;
    next[n].info.lower_is_better = l->lower_is_better != 0;
    snprintf(next[n].info.title, sizeof(next[n].info.title), "%s", l->title ? l->title : "");
    memcpy(next[n].md5, sum, sizeof(sum));
    n++;
  }
  // what the previous set had and this one does not leaves rcheevos too
  for(i = 0; i < lb_n; i++) {
    for(j = 0; j < n && next[j].info.id != lb_set[i].info.id; j++) ;
    if(j == n) rc_runtime_deactivate_lboard(rt, lb_set[i].info.id);
  }
  taskENTER_CRITICAL();
  memcpy(lb_set, next, n * sizeof(lb_set[0]));
  lb_n = n;
  taskEXIT_CRITICAL();
  return n;
}

/* Activates the core achievements and fills the title table. A condition that
   rcheevos rejects is reported, it would otherwise never fire in silence. An
   achievement whose condition did not change keeps running with its hit counts:
   rcheevos would reset it when given the same condition again. */
static int activate_set(rc_runtime_t *rt, const rc_api_fetch_game_data_response_t *r) {
  static entry_t next[RA_PATCH_MAX];   // com_task only, too large for its stack
  unsigned i, j, n = 0, rejected = 0, kept = 0;
  size_t pool_size = 0;
  char *pool, *at;
  deferred_ach_n = 0;

  // one block for all titles and descriptions of the new set, each cut to what the
  // menu shows
  for(i = 0; i < r->num_achievements; i++)
    if(core_item(&r->achievements[i]))
      pool_size += strnlen(r->achievements[i].title ? r->achievements[i].title : "",
                           RA_PATCH_TITLE_MAX - 1) + 1 +
                   strnlen(r->achievements[i].description ? r->achievements[i].description : "",
                           RA_PATCH_DESC_MAX - 1) + 1;
  at = pool = malloc(pool_size ? pool_size : 1);   // pico_malloc stops the firmware when the heap is out

  // every achievement of the set once: skip what does not count, stop when the
  // table is full, and hand the condition string of the others to rcheevos.
  // Stopping keeps rcheevos and the table in step: an achievement active without
  // a table entry would fire without title and position.
  for(i = 0; i < r->num_achievements; i++) {
    const rc_api_achievement_definition_t *a = &r->achievements[i];
    if(!core_item(a)) continue;
    if(n >= RA_PATCH_MAX) {
      // a set that runs only in part is not the set: no hardcore with it
      oom_off++;
      debugf("RA: more than %u achievements, the rest is ignored, softcore with this set", (unsigned)RA_PATCH_MAX);
      break;
    }
    // an entry of the running set with the same id and condition stays as it is
    unsigned char sum[16];
    md5_of(a->definition ? a->definition : "", sum);
    for(j = 0; j < set_n && !(set[j].id == a->id && memcmp(set[j].md5, sum, sizeof(sum)) == 0); j++) ;
    if(j < set_n) {
      kept++;
    } else {
      // rcheevos parses the condition string and checks it from the next frame on,
      // it fires only after the condition was false once. NULL, 0 are the Lua
      // arguments, unused. A string it cannot parse is not activated and does not
      // enter the table.
      int rv = rc_runtime_activate_achievement(rt, a->id, a->definition, NULL, 0);
      // without memory it enters the table and comes once the parsed set is freed
      if(rv == RC_OUT_OF_MEMORY) deferred_ach[deferred_ach_n++] = n;
      else if(rv != RC_OK) {
        debugf("RA: condition %u (%s) rejected, code %d, it will never fire",
               (unsigned)a->id, a->title ? a->title : "", rv);
        rejected++;
        continue;
      }
    }
    // keep id, points, title, description and condition md5, title and description
    // cut and in the block
    size_t tl = strnlen(a->title ? a->title : "", RA_PATCH_TITLE_MAX - 1);
    size_t dl = strnlen(a->description ? a->description : "", RA_PATCH_DESC_MAX - 1);
    next[n].id     = a->id;
    next[n].points = a->points;
    memcpy(at, a->title ? a->title : "", tl);
    at[tl] = 0;
    next[n].title = at;
    at += tl + 1;
    memcpy(at, a->description ? a->description : "", dl);
    at[dl] = 0;
    next[n].desc = at;
    at += dl + 1;
    memcpy(next[n].md5, sum, sizeof(sum));
    n++;
  }

  // what the previous set had and this one does not carry leaves rcheevos too,
  // so nothing fires without an entry in the table
  for(i = 0; i < set_n; i++) {
    for(j = 0; j < n && next[j].id != set[i].id; j++) ;
    if(j == n) rc_runtime_deactivate_achievement(rt, set[i].id);
  }

  // the table goes out in one piece: other tasks read it without a lock, and on
  // this single core a change with interrupts off cannot be seen half done. The
  // progress starts empty, the next update fills it.
  char *old_pool;
  taskENTER_CRITICAL();
  memcpy(set, next, n * sizeof(set[0]));
  set_n = n;
  memset(live, 0, sizeof(live));
  old_pool  = text_pool;
  text_pool = pool;
  taskEXIT_CRITICAL();
  free(old_pool);               // no reader can hold it: they copy with interrupts off

  // the leaderboards
  unsigned lb_kept, lb_rejected;
  deferred_lb_n = 0;
  deferred_rp   = false;
  unsigned lbs = activate_lboards(rt, r, &lb_kept, &lb_rejected);

  // rich presence: the script of this set, an empty one switches it off. A script
  // rcheevos rejects leaves the previous one running, the log says so.
  int rp = activate_richpresence(rt, r->rich_presence_script);
  if(rp != RC_OK && !deferred_rp)
    debugf("RA: rich presence script rejected, code %d", rp);

  debugf("RA: set for '%s': %u achievements active (%u kept running), %u rejected, %u later, "
         "%u leaderboards active (%u kept running), %u rejected, %u later, rich presence %s",
         r->title ? r->title : "?", n - deferred_ach_n, kept, rejected, deferred_ach_n,
         lbs, lb_kept, lb_rejected, deferred_lb_n,
         deferred_rp ? "later" : rp != RC_OK ? "rejected" : rp_on ? "on" : "none");
  return (int)n;
}

void ra_patch_rom_start(const char *name) {
  char hex[RA_GAMES_HASH_LEN + 1];
  rom_started = xTaskGetTickCount();   // before rom_pending: a reader never sees an old start
  rom_pending = true;
  ra_task_hardcore_block(RA_HC_BLOCK_ROM, true);
  // the name's hash right away, not the name: a long name (up to 255 characters)
  // would not fit a small buffer, and the hash is all the arcade rule needs. Made
  // in a local and published whole: the 15 s timeout settle in com_task can read
  // rom_name_hash while menu_task is in here
  if(name) ra_games_name_hash(name, hex);
  else     hex[0] = 0;
  taskENTER_CRITICAL();
  memcpy(rom_name_hash, hex, sizeof(rom_name_hash));
  rom_sum_valid = false;
  rom_bad       = false;
  stream_seq    = pick_seq;
  taskEXIT_CRITICAL();
  mbedtls_sha256_init(&rom_sha);
  rom_hashing = mbedtls_sha256_starts(&rom_sha, 0) == 0;
}

void ra_patch_rom_data(const void *data, unsigned len) {
  if(rom_hashing && mbedtls_sha256_update(&rom_sha, data, len) != 0) rom_hashing = false;
}

bool ra_patch_rom_end(const unsigned char want[32]) {
  rom_sum_valid = rom_hashing && mbedtls_sha256_finish(&rom_sha, rom_sum) == 0;
  mbedtls_sha256_free(&rom_sha);
  rom_hashing = false;
  rom_pending = false;
  // a content that is not what the footer names is no game at all: the core stays
  // in reset, and a guess by the file's name would start a session for nothing
  rom_bad = rom_sum_valid && want && memcmp(rom_sum, want, 32) != 0;
  if(rom_sum_valid)
    debugf("RA: ROM image SHA-256 %02x%02x%02x%02x...%s", rom_sum[0], rom_sum[1], rom_sum[2], rom_sum[3],
           rom_bad ? ", not the footer's: damaged, no game" : "");
  else
    debugf("RA: hashing the ROM image failed, it counts as unknown");
  // the core still waits in reset here: the bits settle applies hold from the first frame
  ra_patch_settle();
  return rom_bad;
}

void ra_patch_rom_rejected(void) {
  // only while no game is decided: later the core keeps the ROM it runs
  if(!settled) rom_bad = true;
}

void ra_patch_rom_gone(void) {
  // no FatFs and no settle here: this runs in menu_task (a file ejected in the
  // OSD, sdc_image_open). Only the ROM block bit moves, and without a name the
  // next settle applies its "no ROM" rule. A file that replaces the ROM, or fails
  // to, does not come here: the core keeps the ROM it has until a new stream
  // starts, and rom_start takes over then.
  if(rom_hashing) mbedtls_sha256_free(&rom_sha);
  rom_hashing = false;
  // published whole, as rom_start does: the 15 s timeout settle in com_task may
  // read these while menu_task is in here
  taskENTER_CRITICAL();
  rom_pending      = false;
  rom_sum_valid    = false;
  rom_name_hash[0] = 0;
  taskEXIT_CRITICAL();
  ra_task_hardcore_block(RA_HC_BLOCK_ROM, true);
}

bool ra_patch_rom_pending(void) { return rom_pending; }
bool ra_patch_rom_streaming(unsigned limit_ms) {
  return rom_pending && (TickType_t)(xTaskGetTickCount() - rom_started) < pdMS_TO_TICKS(limit_ms);
}
bool ra_patch_foreign_rom(void) { return foreign_rom; }
const ra_game_t *ra_patch_restart_to(void) { return restart_to; }

void ra_patch_restart_cancel(void) {
  taskENTER_CRITICAL();
  restart_to        = NULL;
  restart_committed = false;
  stream_seq        = pick_seq;   // the dropped pick holds no later settle back
  taskEXIT_CRITICAL();
}

bool ra_patch_restart_commit(const ra_game_t *g) {
  bool fixed;
  taskENTER_CRITICAL();   // a pick in the menu task must not slip in between
  fixed = g && restart_to == g;
  if(fixed) restart_committed = true;
  taskEXIT_CRITICAL();
  return fixed;
}

bool ra_patch_pick_other_board(const char *name) {
  const ra_game_t *g = ra_games_by_file(name);
  bool taken = false;
  taskENTER_CRITICAL();   // the settle may change the target meanwhile, see ra_patch_settle()
  if(restart_committed)
    // the switch is under way: a game of another board comes too late and is
    // dropped. Any other file goes on to sdc_image_open(), which waits for the card
    // lock: it streams only if the switch fails, else the Pico restarts first
    taken = g && board && g->board != board;
  else if(g && board && g->board != board) {
    restart_to = g;       // the newest pick wins
    pick_seq++;           // also over a stream that started before it, see the settle
    taken = true;
  }
  taskEXIT_CRITICAL();
  return taken;
}

void ra_patch_board(unsigned char b) { board = b; }
bool ra_patch_settled(void) { return settled; }

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

/* Hands a set to com_task. One it has not taken yet gives way, the newer one counts.
   fp is the fingerprint of the text in body[] it was parsed from, for the parts
   that are read from there later, see activate_deferred(). */
static void hand_over(rc_api_fetch_game_data_response_t *r, const unsigned char *fp) {
  rc_api_fetch_game_data_response_t *old;
  memcpy(handed_fp, fp, sizeof(handed_fp));
  if(xQueueReceive(handover, &old, 0) == pdTRUE) discard(old);
  xQueueSend(handover, &r, 0);
}

/* Reads the set from the card, parses it and hands it to com_task. From settle in
   com_task before the RA task runs, and from the RA task itself when the set is
   read once more (ra_patch_set_again), so the two never share body[]. True when
   a set with a valid tag went over. Otherwise the card does not hold the set the
   server sent last, so the fingerprint is dropped: the next set from the server
   is written and handed over again instead of passing as unchanged. */
static bool read_card(void) {
  rc_api_fetch_game_data_response_t *r;
  unsigned n;

  unsigned char fp[16];

  card_fp_valid = card_fp_runs = false;
  if(!handover || !card_read()) return false;
  fingerprint(body, body_len, fp);   // over the file as it lies on the card, before the parse
  if(!(r = parse_new(&n))) return false;
  memcpy(card_fp, fp, sizeof(card_fp));
  card_fp_runs = true;
  note_warning(r);          // until the server's set arrives, the card's says it
  // only a set with a valid tag counts as the one the server sent last: its
  // fingerprint lets an unchanged server set pass, and hardcore may use it. An
  // untagged or edited one plays softcore until the server's set replaces it.
  if(card_verified) {
    card_fp_valid = true;
    ra_task_hardcore_block(RA_HC_BLOCK_SET, false);
  } else
    debugf("RA: set on the card has no valid tag, softcore until the server's set arrives");
  hand_over(r, fp);
  debugf("RA: set from the card: %u core achievements", n);
  return card_verified;
}

/* Copies the file at from to the file at to, through body[], and removes from.
   Copy and not rename: FatFs' rename registers the new entry before it removes
   the old one, and a power cut in between leaves two names on one cluster chain,
   which a later unlink of one name would free under the other. A copy never
   shares clusters, and a torn copy heals on the next boot: the root file is still
   there, FA_CREATE_ALWAYS overwrites the folder's file, and the root file goes
   only after a complete write. Under sdc_lock, before the RA task exists, so
   body[] is free. */
static void move_file(const char *from, const char *to) {
  FIL f;
  UINT n = 0, put = 0;
  FSIZE_t size;
  FRESULT r = f_open(&f, from, FA_READ);
  // f_stat found the name: a folder of that name (FR_NO_FILE) or a card error
  if(r != FR_OK) { debugf("RA: %s not opened (error %d), not moved", from, (int)r); return; }
  size = f_size(&f);
  // a file that does not fit could never be read as a set either, it stays where
  // it is and the log says so on every boot
  if(size >= sizeof(body)) {
    f_close(&f);
    debugf("RA: %s is %lu bytes, too large to move, left in place", from, (unsigned long)size);
    return;
  }
  r = f_read(&f, body, (UINT)size, &n);
  f_close(&f);
  if(r != FR_OK || n != size) { debugf("RA: %s not read (error %d), not moved", from, (int)r); return; }
  r = f_open(&f, to, FA_WRITE | FA_CREATE_ALWAYS);
  if(r == FR_OK) {
    r = f_write(&f, body, n, &put);
    FRESULT c = f_close(&f);      // f_close writes the last sector, its result counts
    if(r == FR_OK) r = c;
    if(r == FR_OK && put != n) r = FR_DISK_ERR;
  }
  if(r != FR_OK) { debugf("RA: %s not written (error %d), %s stays", to, (int)r, from); return; }
  // the copy is whole: the original goes. A root file that stays (read-only
  // attribute, write error) is copied again on the next boot, which is harmless
  r = f_unlink(from);
  if(r == FR_OK) debugf("RA: %s moved to %s", from, to);
  else           debugf("RA: %s copied to %s, not removed (error %d)", from, to, (int)r);
}

/* The card files of the firmware before this one lie in the root and belong to
   Galaga (RA_GAMES_V1_ID), the only game it knew. They go into Galaga's folder
   on every boot they are found, whatever core runs: the set's tag covers id and
   content and not the name, so a card that had hardcore keeps it. Order mac,
   json, unlocked: a power cut after the mac reads as "no set" next boot, which
   is harmless, and the json follows. A root file is copied whenever it is there,
   over a folder copy too: a torn copy (a power cut leaves the folder's entry with
   size 0) is made whole this way, and after a downgrade and upgrade the root
   files, which the old firmware wrote last, win over the folder's. When nothing
   is left this costs three f_stat calls per boot. */
static void migrate_card(void) {
  static const struct { const char *root; const char *name; } files[] = {
    { "/sd/ra_patch.mac",    RA_GAMES_SET_MAC },
    { "/sd/ra_patch.json",   RA_GAMES_SET_FILE },
    { "/sd/ra_unlocked.txt", RA_GAMES_STATE_FILE },
  };
  char to[RA_GAMES_PATH_MAX];
  FILINFO fi;
  bool folder = false;
  unsigned i;

  sdc_lock();
  f_unlink("/sd/ra_patch.json.new");   // a partial write of the old firmware, worthless
  for(i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    if(f_stat(files[i].root, &fi) != FR_OK) continue;
    if(!folder && ra_games_mkdir(RA_GAMES_V1_ID) != FR_OK) {
      debugf("RA: no folder for the card files of game %u, %s stays", RA_GAMES_V1_ID, files[i].root);
      break;
    }
    folder = true;
    ra_games_path(RA_GAMES_V1_ID, files[i].name, to, sizeof(to));
    move_file(files[i].root, to);
  }
  sdc_unlock();
}

void ra_patch_settle(void) {
  ra_ident_t ident;
  unsigned char sum[32];
  char name_hash[RA_GAMES_HASH_LEN + 1];
  bool sum_valid, streaming, bad;
  // the inputs whole: rom_start in menu_task may be writing them when the 15 s
  // timeout of the main loop brings com_task here
  taskENTER_CRITICAL();
  memcpy(sum, rom_sum, sizeof(sum));
  memcpy(name_hash, rom_name_hash, sizeof(name_hash));
  sum_valid = rom_sum_valid;
  streaming = rom_pending;
  bad       = rom_bad;
  taskEXIT_CRITICAL();
  ra_games_select(board, sum_valid ? sum : NULL, name_hash[0] ? name_hash : NULL, streaming, &ident);
  if(bad) {
    // a damaged ROM, or none for an old file: no game, neither by the name nor by
    // the board, so the RA task asks the server nothing ("RA: NO GAME")
    memset(&ident, 0, sizeof(ident));
    ident.rom_label = "ROM damaged or old";
  }

  if(settled) {
    // a ROM picked in the OSD after the start: identity, session and card files
    // stay, the two block bits and the label of the Version dialog follow the new
    // file. A file of another game switches the set off, ra_patch_apply_pending()
    // takes it out of rcheevos, until the boot's ROM is back in the core.
    bool same = ident.game == game && !strcmp(ident.hash, game_hash);
    rom_label = ident.rom_label;
    ra_task_hardcore_block(RA_HC_BLOCK_ROM,  !(same && ident.rom_ok));
    ra_task_hardcore_block(RA_HC_BLOCK_GAME, !ident.board_ok || !same || table_wrong);
    const char *what = ident.game ? ident.game->set : ident.hash[0] ? "a file the table does not know" : "gone";
    if(!same && !foreign_rom && !game_hash[0])
      // no game at the start (board unknown, or a table game on the wrong board):
      // the identity is fixed for the boot, nothing can come back
      debugf("RA: the ROM is now %s, but no game was decided at the start: reboot with this ROM to play it", what);
    else if(!same && !foreign_rom)
      debugf("RA: the ROM is now %s, not the game's: achievements off until it is back", what);
    else if(same && foreign_rom && game_hash[0])
      debugf("RA: the game's ROM is back, its set is read again");
    foreign_rom = !same;
    // the newest pick wins: a known ROM of another game of this board, proven by
    // its digest, becomes the target of a restart of the Pico, which makes it the
    // boot's game with its own set (restart_step() in main.c). The boot's own ROM,
    // or any other file, takes a pending target back. Only when a game was decided
    // at the start, else the identity was never set up and the log line above says
    // what to do.
    const ra_game_t *to = !same && ident.game && ident.rom_ok && ident.board_ok && game_hash[0] ? ident.game : NULL;
    const ra_game_t *was;
    bool later;
    taskENTER_CRITICAL();   // the menu may pick a game of another board meanwhile
    was   = restart_to;
    later = stream_seq != pick_seq;   // a game of another board was picked after this stream began
    if(!restart_committed && !later) restart_to = to;
    taskEXIT_CRITICAL();
    if(later)
      debugf("RA: %s was picked after this ROM, it stays the target", was ? was->set : "a game of another board");
    else if(to && to != was)
      debugf("RA: the ROM is now %s, a game of this board: restart for its achievements", to->set);
    else if(!to && was)
      debugf("RA: no restart for %s, the ROM picked last plays here", was->set);
    return;
  }
  settled     = true;
  game        = ident.game;
  game_id     = ident.id;
  resolved_id = 0;
  rom_label   = ident.rom_label;
  snprintf(game_hash, sizeof(game_hash), "%s", ident.hash);
  debugf("RA: board %u, ROM %s: game %s, id %u, hash %s", board, rom_label,
         game ? game->set : ident.hash[0] ? "unknown to the table" : "none",
         game_id, game_hash[0] ? game_hash : "none");
  if(game && !ident.board_ok)
    debugf("RA: %s runs on board %u, not on this one: not played", game->title, game->board);
  ra_task_hardcore_block(RA_HC_BLOCK_ROM,  !ident.rom_ok);
  ra_task_hardcore_block(RA_HC_BLOCK_GAME, !ident.board_ok);
  migrate_card();
  // the card set, only with an id: a fallback identity has no folder until the
  // server resolved the hash, and it is softcore anyway, the server's set comes
  // after the login
  if(game_id) {
    sdc_lock();
    FRESULT r = ra_games_mkdir(game_id);
    sdc_unlock();
    if(r != FR_OK) debugf("RA: folder for game %u not made (error %d)", game_id, (int)r);
    read_card();
  }
}

void ra_patch_resolved(unsigned server_id) {
  const ra_game_t *g;
  FRESULT r;
  if(game_id) {
    // a table entry: the table is wrong, softcore until a developer fixes it, its
    // id is kept so the card files and the session stay consistent
    if(server_id != game_id) {
      debugf("RA: table says %u, server says %u, softcore until the table is fixed", game_id, server_id);
      table_wrong = true;
      ra_task_hardcore_block(RA_HC_BLOCK_GAME, true);
    }
    return;
  }
  if(!server_id) {
    debugf("RA: the server does not know hash %s, no achievements", game_hash);
    return;
  }
  // a fallback identity: a table game of another board is not played, its set
  // would fire on unrelated bytes. On a board the table knows, only one of its
  // own games is: the board's ROMs are table games, so a name the table does not
  // know is a renamed or foreign file, and a set the table does not know would
  // fire on this board's RAM the same way. A board without a table entry, a new
  // core with a new ROM, plays whatever the server says, once its header was
  // read: board 0 is no board, a set adopted then would run against whatever
  // core answers later. What is stored gets its folder made, so card_write and
  // ra_state_save find it.
  unsigned char b = board;   // read once, the poll may adopt a header meanwhile
  g = ra_games_by_id(server_id);
  if(!b) {
    debugf("RA: the server says game %u, but the board is unknown (no RAM mirror header yet): not played",
           server_id);
    return;
  }
  if(g && g->board != b) {
    debugf("RA: the server says game %u (%s), which runs on board %u, not %u: not played",
           server_id, g->title, g->board, b);
    return;
  }
  if(!g && ra_games_by_board(b)) {
    debugf("RA: the server says game %u, which the table does not know on board %u: not played",
           server_id, b);
    return;
  }
  resolved_id = server_id;
  sdc_lock();
  r = ra_games_mkdir(server_id);
  sdc_unlock();
  debugf("RA: the server knows the ROM as game %u%s", server_id, r == FR_OK ? "" : ", its card folder could not be made");
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
  // the same set as on the card: nothing to parse, write or hand over, the set
  // that runs came from the card at start. Compared before the parse, which
  // would need as much memory again as a large set already takes.
  fingerprint(body, body_len, sum);
  bool same = card_fp_runs && memcmp(sum, card_fp, sizeof(sum)) == 0;
  if(same && card_fp_valid) {
    debugf("RA: set unchanged on the server");
    return 0;
  }
  // The set that runs came from the card without this Pico's tag, a new Pico or a
  // card from another one, and it is the server's set. Parsing it again is not
  // needed, and a large set leaves no memory for it. The card gets the server's
  // file with this Pico's tag, and the set may count in hardcore, which starts with
  // a reset of the game as for any set from the server.
  if(same) {
    if(card_write()) {
      card_fp_valid = true;
      debugf("RA: the set on the card is the server's, now with this Pico's tag");
    } else
      debugf("RA: the set on the card is the server's, its tag could not be written");
    ra_task_hardcore_block(RA_HC_BLOCK_SET, false);
    return 0;
  }
  if(!(r = parse_new(&n))) return -1;
  note_warning(r);
  // the card first, so a power cut after this leaves the new set there too
  if(card_write()) {
    memcpy(card_fp, sum, sizeof(card_fp));
    card_fp_valid = card_fp_runs = true;
    debugf("RA: set from the server: %u core achievements, kept on the card", n);
  } else {
    card_fp_runs = false;   // the card no longer holds the set that runs
    debugf("RA: set from the server: %u core achievements, only in memory", n);
  }
  hand_over(r, sum);
  // straight from the server over TLS: this set may count in hardcore
  ra_task_hardcore_block(RA_HC_BLOCK_SET, false);
  return 1;
}

void ra_patch_core_reset(void) { reset_due = true; }

bool ra_patch_set_again(void) {
  if(!set_again) return true;
  set_again = false;
  return read_card();
}

/* Activates what activate_set() deferred for lack of memory, once the parsed set
   is freed: from the set's text in body[], one definition at a time on the heap,
   found and decoded by rcheevos' own JSON reader as rc_api would. body[] must
   still hold the text the set was parsed from, and each definition must have the
   md5 it had there, else it stays off, and the log names what stays off. */
static void activate_deferred(rc_runtime_t *rt) {
  rc_json_field_t top[] = { RC_JSON_NEW_FIELD("PatchData") };
  rc_json_field_t pd[]  = { RC_JSON_NEW_FIELD("Leaderboards"), RC_JSON_NEW_FIELD("RichPresencePatch"),
                            RC_JSON_NEW_FIELD("Achievements") };
  rc_json_field_t lbf[] = { RC_JSON_NEW_FIELD("ID"), RC_JSON_NEW_FIELD("Mem") };
  rc_json_field_t af[]  = { RC_JSON_NEW_FIELD("ID"), RC_JSON_NEW_FIELD("MemAddr") };
  rc_json_iterator_t it;
  rc_json_field_t arr;
  unsigned char fp[16], sum[16];
  uint32_t num, id;
  unsigned i;
  if(!deferred_ach_n && !deferred_lb_n && !deferred_rp) return;

  // the text the set came from, else nothing is read from it
  fingerprint(body, body_len, fp);
  if(memcmp(fp, handed_fp, sizeof(fp))) goto out;
  it.json = body; it.end = body + body_len;
  if(!rc_json_get_array_entry_object(top, 1, &it) || !top[0].value_start) goto out;
  it.json = top[0].value_start; it.end = top[0].value_end;
  if(!rc_json_get_array_entry_object(pd, 3, &it)) goto out;

  // the achievements first: each deferred one by its id, its condition decoded alone
  if(deferred_ach_n && rc_json_get_optional_array(&num, &arr, &pd[2], "Achievements")) {
    it.json = arr.value_start; it.end = arr.value_end;
    while(deferred_ach_n && rc_json_get_array_entry_object(af, 2, &it)) {
      rc_buffer_t buf;
      const char *mem = NULL;
      int rv = RC_INVALID_STATE;
      if(!rc_json_get_unum(&id, &af[0], "ID")) continue;
      for(i = 0; i < deferred_ach_n && set[deferred_ach[i]].id != id; i++) ;
      if(i == deferred_ach_n) continue;
      const entry_t *e = &set[deferred_ach[i]];
      rc_buffer_init(&buf);
      if(rc_json_get_string(&mem, &buf, &af[1], "MemAddr") && mem) {
        md5_of(mem, sum);
        if(!memcmp(sum, e->md5, sizeof(sum)))
          rv = rc_runtime_activate_achievement(rt, id, mem, NULL, 0);
      }
      rc_buffer_destroy(&buf);
      if(rv != RC_OK) {
        oom_off++;
        debugf("RA: achievement %u (%s) stays off, code %d", (unsigned)id, e->title, rv);
      } else
        debugf("RA: achievement %u (%s) active", (unsigned)id, e->title);
      deferred_ach[i] = deferred_ach[--deferred_ach_n];
    }
  }

  // the leaderboards: each deferred one by its id, its definition decoded alone
  if(deferred_lb_n && rc_json_get_optional_array(&num, &arr, &pd[0], "Leaderboards")) {
    it.json = arr.value_start; it.end = arr.value_end;
    while(deferred_lb_n && rc_json_get_array_entry_object(lbf, 2, &it)) {
      rc_buffer_t buf;
      const char *mem = NULL;
      int rv = RC_INVALID_STATE;
      if(!rc_json_get_unum(&id, &lbf[0], "ID")) continue;
      for(i = 0; i < deferred_lb_n && deferred_lb[i].info.id != id; i++) ;
      if(i == deferred_lb_n) continue;
      rc_buffer_init(&buf);
      if(rc_json_get_string(&mem, &buf, &lbf[1], "Mem") && mem) {
        md5_of(mem, sum);
        if(!memcmp(sum, deferred_lb[i].md5, sizeof(sum)))
          rv = rc_runtime_activate_lboard(rt, id, mem, NULL, 0);
      }
      rc_buffer_destroy(&buf);
      if(rv != RC_OK) {
        oom_off++;
        debugf("RA: leaderboard %u (%s) stays off, code %d", (unsigned)id, deferred_lb[i].info.title, rv);
      } else if(lb_n < RA_PATCH_LB_MAX) {
        taskENTER_CRITICAL();
        lb_set[lb_n++] = deferred_lb[i];
        taskEXIT_CRITICAL();
        debugf("RA: leaderboard %u (%s) active", (unsigned)id, deferred_lb[i].info.title);
      }
      deferred_lb[i] = deferred_lb[--deferred_lb_n];
    }
  }

  // the rich presence script
  if(deferred_rp) {
    rc_buffer_t buf;
    const char *script = NULL;
    int rv = RC_INVALID_STATE;
    rc_buffer_init(&buf);
    if(rc_json_get_string(&script, &buf, &pd[1], "RichPresencePatch") && script) {
      md5_of(script, sum);
      if(!memcmp(sum, deferred_rp_md5, sizeof(sum))) {
        rv = rc_runtime_activate_richpresence(rt, script, NULL, 0);
        if(rv == RC_OUT_OF_MEMORY) drop_half_richpresence(rt);
        if(rv == RC_OK) {
          memcpy(rp_md5, sum, sizeof(rp_md5));
          rp_on = true;
        }
      }
    }
    rc_buffer_destroy(&buf);
    if(rv != RC_OK) oom_off++;
    debugf("RA: rich presence %s, code %d", rv == RC_OK ? "active" : "stays off", rv);
    deferred_rp = false;
  }

out:
  oom_off += deferred_ach_n + deferred_lb_n + (deferred_rp ? 1 : 0);
  for(i = 0; i < deferred_ach_n; i++)
    debugf("RA: achievement %u (%s) stays off, the set's text was not there", set[deferred_ach[i]].id, set[deferred_ach[i]].title);
  for(i = 0; i < deferred_lb_n; i++)
    debugf("RA: leaderboard %u (%s) stays off, the set's text was not there", (unsigned)deferred_lb[i].info.id, deferred_lb[i].info.title);
  if(deferred_rp) debugf("RA: rich presence stays off, the set's text was not there");
  deferred_ach_n = 0;
  deferred_lb_n  = 0;
  deferred_rp    = false;
  debugf("RA: after the deferred parts, SDK heap free %lu", (unsigned long)getFreeHeap());
}

unsigned ra_patch_apply_pending(rc_runtime_t *rt) {
  rc_api_fetch_game_data_response_t *r;
  unsigned what = 0;
  // after a reset of the core: hit counts, leaderboards and rich presence anew
  if(reset_due) {
    reset_due = false;
    rc_runtime_reset(rt);
    debugf("RA: core reset, achievement state starts over");
    what |= RA_PATCH_RESET;
  }
  // the ROM in the core is another game's: the set leaves rcheevos, an empty set
  // through activate_set() deactivates every achievement and leaderboard and
  // drops the rich presence, and nothing is handed over meanwhile. No set is
  // proven for what runs now, so RA_HC_BLOCK_SET holds until one is read again.
  // Back with the boot's ROM, the RA task, which owns body[] now, reads the card
  // set anew and hands it over like the first time.
  if(foreign_rom != set_off) {
    set_off = foreign_rom;
    if(set_off) {
      static const rc_api_fetch_game_data_response_t none;
      debugf("RA: the set is switched off, the ROM in the core is not the game's");
      rc_runtime_reset(rt);
      activate_set(rt, &none);
      ra_task_hardcore_block(RA_HC_BLOCK_SET, true);
      what |= RA_PATCH_NEW_SET;
    } else {
      set_again = true;
      ra_task_wake();
    }
  }
  if(!set_off && handover && xQueueReceive(handover, &r, 0) == pdTRUE) {
    oom_off = 0;
    activate_set(rt, r);
    discard(r);             // rcheevos has copied the conditions
    activate_deferred(rt);  // what did not fit beside the parsed set
    // a set that runs only in part is not the set: no hardcore with it
    if(oom_off) debugf("RA: %u parts of the set stay off, softcore with this set", oom_off);
    ra_task_hardcore_block(RA_HC_BLOCK_SIZE, oom_off != 0);
    what |= RA_PATCH_NEW_SET;
  }
  return what;
}
