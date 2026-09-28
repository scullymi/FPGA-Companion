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
#include "rc_runtime_types.h"   // rc_trigger_t and its states, for the challenge indicator
#include "rcheevos/src/rhash/md5.h"
#include "mbedtls/sha256.h"
#include <ff.h>
#include "debug.h"
#include "sdc.h"
#include "ra_net.h"
#include "ra_patch.h"
#include "ra_task.h"
#include "ra_mac.h"

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
// The ROM file hardcore allows, as scripts/make_galaga_rom.sh builds it from the MAME
// set "galaga" (every chip checked against MAME's SHA-1): SHA-256 with the 54xx, and
// with its 1024 bytes as zeros when namco54.zip is missing. RetroAchievements only
// knows the game by the set's name, this also proves the content, so a patched ROM
// (more lives, say) plays in softcore only.
static const unsigned char rom_known[][32] = {
  { 0xaa,0xf7,0xa7,0x25,0x6f,0x8c,0x4e,0x97,0xb3,0x1f,0x05,0x3e,0x68,0x8f,0x24,0xcb,
    0xb3,0x40,0x75,0xa2,0x6a,0xc7,0x1f,0xf0,0x84,0x76,0x51,0xae,0x93,0xb2,0xaf,0x47 },
  { 0xec,0x21,0xe5,0x4d,0xaa,0x09,0xf7,0x8b,0x2f,0x58,0xab,0x06,0x0f,0x5c,0xdf,0xbd,
    0x29,0xd5,0x0b,0x29,0x81,0x60,0x41,0xc6,0xca,0xe3,0x82,0x6c,0xb2,0xda,0xbc,0xd5 },
};
static mbedtls_sha256_context rom_sha;   // the ROM image while it streams, com_task only
static bool                   rom_hashing;

#define RA_PATCH_BODY_MAX   40960        /**< a whole set, e.g. Galaga has about 15.7 KB */
#define RA_PATCH_MAX        64           /**< achievements kept per set, e.g. Galaga 17 */
#define RA_PATCH_FILE       "/sd/ra_patch.json"   /**< the server's reply to r=patch, kept on the card */
#define RA_PATCH_TMP        RA_PATCH_FILE ".new"  /**< a new set while it is written, then it takes the place of the old */
#define RA_PATCH_MAC        "/sd/ra_patch.mac"    /**< "g20k-s1 <tag>": the set file's tag with the device key */
#define RA_PATCH_MAC_LABEL  "g20k-s1"             /**< what the set's tag is made over, keeps it apart from other tags */
/** "Warning: Unknown Emulator", which the server adds for clients it does not
   know. Not an achievement of the game. */
#define RA_PATCH_WARNING_ID 101000001u

// the set as it was read from the card or received from the server, for
// rcheevos' parser. Static, 40 KB would not fit on a task's stack. RA task only.
static char     body[RA_PATCH_BODY_MAX];
static unsigned body_len;

// what the set on the card amounts to, to tell whether the server's is a new
// one: id, title and condition of every core achievement, the rich presence
// script and the leaderboards. The rest of the reply may change from request to
// request.
static unsigned char card_fp[16];
static bool          card_fp_valid;

// the core was reset, rcheevos starts over before the next frame. Set by any
// task, cleared by com_task.
static volatile bool reset_due;

// the set file read from the card carried a tag that checks out: it is the set
// this Pico received from the server, not one edited on a computer
static bool card_verified;

/* The data the set's tag is made over: the game id (4 bytes, little endian) and the
   file as it lies on the card. Returns its length. */
static char tag_buf[4 + RA_PATCH_BODY_MAX];   // RA task only, too large for its stack
static unsigned set_tag_data(const char *data, unsigned len) {
  unsigned id = ra_game_id();
  tag_buf[0] = (char)id; tag_buf[1] = (char)(id >> 8); tag_buf[2] = (char)(id >> 16); tag_buf[3] = (char)(id >> 24);
  memcpy(tag_buf + 4, data, len);
  return 4 + len;
}
static bool set_tag(const char *data, unsigned len, char *hex) {
  return ra_mac_tag(RA_PATCH_MAC_LABEL, tag_buf, set_tag_data(data, len), hex);
}
static bool set_check(const char *data, unsigned len, const char *hex) {
  return ra_mac_check(RA_PATCH_MAC_LABEL, tag_buf, set_tag_data(data, len), hex);
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
  char          title[RA_PATCH_TITLE_MAX];  /**< its title, cut to fit */
  const char   *desc;                      /**< its description, in desc_pool */
  unsigned char md5[16];                   /**< md5 of its condition, an unchanged one keeps running */
} entry_t;
static entry_t  set[RA_PATCH_MAX];
static unsigned set_n;
// the descriptions of the active set, one block sized to the set: Galaga's 17
// take about 2 KB, a table of 64 fixed 256-byte fields would take 16 KB
static char    *desc_pool;

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

// the rich presence script that runs, by its md5. com_task only.
static unsigned char rp_md5[16];
static bool          rp_on;

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

bool ra_patch_lboard(unsigned id, ra_patch_lboard_t *out) {
  bool ok = false;
  unsigned i;
  taskENTER_CRITICAL();
  for(i = 0; i < lb_n; i++)
    if(lb_set[i].info.id == id) { *out = lb_set[i].info; ok = true; break; }
  taskEXIT_CRITICAL();
  return ok;
}

bool ra_patch_item(unsigned i, ra_patch_item_t *out) {
  bool ok = false;
  // one copy with interrupts off, so an entry is never seen half replaced
  taskENTER_CRITICAL();
  if(i < set_n) {
    out->id     = set[i].id;
    out->points = set[i].points;
    memcpy(out->title, set[i].title, sizeof(out->title));
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
  // its tag, one line "g20k-s1 <64 hex>"
  char mac_line[16 + RA_MAC_HEX];
  bool mac_read = false;
  { FIL m;
    UINT mgot = 0;
    if(f_open(&m, RA_PATCH_MAC, FA_READ) == FR_OK) {
      mac_read = f_read(&m, mac_line, sizeof(mac_line) - 1, &mgot) == FR_OK;
      mac_line[mgot] = 0;
      f_close(&m);
    }
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
    // the tag over the file as it lies on the card, before anything is changed in body
    card_verified = false;
    if(mac_read && !strncmp(mac_line, RA_PATCH_MAC_LABEL " ", strlen(RA_PATCH_MAC_LABEL) + 1)) {
      char *hex = mac_line + strlen(RA_PATCH_MAC_LABEL) + 1;
      hex[strcspn(hex, "\r\n")] = 0;
      card_verified = set_check(body, body_len, hex);
    }
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
   new one. card_read() puts a file back that was left under the second name. The
   tag goes first away and last in: a power cut in between leaves a set without a
   tag, which only costs hardcore until the server is reached. */
static bool card_write(void) {
  FIL f;
  UINT put = 0;
  FRESULT r;
  char hex[RA_MAC_HEX + 1];
  bool tagged = set_tag(body, body_len, hex);

  sdc_lock();
  f_unlink(RA_PATCH_MAC);         // may not exist
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
  // the tag, when there is a device key to make one
  if(r == FR_OK && tagged && f_open(&f, RA_PATCH_MAC, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    bool ok = f_printf(&f, "%s %s\n", RA_PATCH_MAC_LABEL, hex) > 0;
    if(f_close(&f) != FR_OK || !ok) f_unlink(RA_PATCH_MAC);
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

/* Adds one md5 over a tag, two numbers and up to three strings to sum, as a
   128-bit addition. Addition and not xor: with xor, two equal items cancel out,
   so a set with one achievement doubled and changed would read as unchanged. */
static void fold(unsigned char *sum, char tag, uint32_t id, uint32_t num,
                 const char *s1, const char *s2, const char *s3) {
  md5_state_t md5;
  unsigned char one[16];
  unsigned k, carry = 0;
  md5_init(&md5);
  md5_append(&md5, (const md5_byte_t *)&tag, 1);
  md5_append(&md5, (const md5_byte_t *)&id, sizeof(id));
  md5_append(&md5, (const md5_byte_t *)&num, sizeof(num));
  if(s1) md5_append(&md5, (const md5_byte_t *)s1, (int)strlen(s1) + 1);
  if(s2) md5_append(&md5, (const md5_byte_t *)s2, (int)strlen(s2) + 1);
  if(s3) md5_append(&md5, (const md5_byte_t *)s3, (int)strlen(s3));
  md5_finish(&md5, one);
  for(k = 0; k < 16; k++) {
    carry += (unsigned)sum[k] + one[k];
    sum[k] = (unsigned char)carry;
    carry >>= 8;
  }
}

/* A fingerprint of a parsed set: one md5 per core achievement over id, points,
   title, description and condition, one per leaderboard over id, format, title
   and definition, and one over the rich presence script, all added up, so the
   order in the reply does not matter. Two sets with the same fingerprint play
   and show the same. */
static void fingerprint(const rc_api_fetch_game_data_response_t *r, unsigned char *sum) {
  unsigned i;
  memset(sum, 0, 16);
  for(i = 0; i < r->num_achievements; i++) {
    const rc_api_achievement_definition_t *a = &r->achievements[i];
    if(core_item(a)) fold(sum, 'a', a->id, a->points, a->title, a->description, a->definition);
  }
  for(i = 0; i < r->num_leaderboards; i++) {
    const rc_api_leaderboard_definition_t *l = &r->leaderboards[i];
    fold(sum, 'l', l->id, (uint32_t)l->format, l->title, NULL, l->definition);
  }
  if(r->rich_presence_script && *r->rich_presence_script)
    fold(sum, 'r', 0, 0, NULL, NULL, r->rich_presence_script);
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
  if(rv == RC_OK) {
    memcpy(rp_md5, sum, sizeof(rp_md5));
    rp_on = true;
  } else if(!rt->richpresence || !rt->richpresence->richpresence)
    rp_on = false;          // a parse error keeps the old script, running out of memory does not
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

  // one block for all descriptions of the new set, each cut to what the menu shows
  for(i = 0; i < r->num_achievements; i++)
    if(core_item(&r->achievements[i]))
      pool_size += strnlen(r->achievements[i].description ? r->achievements[i].description : "",
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
      debugf("RA: more than %u achievements, the rest is ignored", (unsigned)RA_PATCH_MAX);
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
      if(rv != RC_OK) {
        debugf("RA: condition %u (%s) rejected, code %d, it will never fire",
               (unsigned)a->id, a->title ? a->title : "", rv);
        rejected++;
        continue;
      }
    }
    // keep id, points, title, description and condition md5. snprintf cuts the
    // title to the table, the description goes into the block.
    size_t dl = strnlen(a->description ? a->description : "", RA_PATCH_DESC_MAX - 1);
    next[n].id     = a->id;
    next[n].points = a->points;
    snprintf(next[n].title, sizeof(next[n].title), "%s", a->title ? a->title : "");
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
  old_pool  = desc_pool;
  desc_pool = pool;
  taskEXIT_CRITICAL();
  free(old_pool);               // no reader can hold it: they copy with interrupts off

  // the leaderboards
  unsigned lb_kept, lb_rejected;
  unsigned lbs = activate_lboards(rt, r, &lb_kept, &lb_rejected);

  // rich presence: the script of this set, an empty one switches it off. A script
  // rcheevos rejects leaves the previous one running, the log says so.
  int rp = activate_richpresence(rt, r->rich_presence_script);
  if(rp != RC_OK)
    debugf("RA: rich presence script rejected, code %d", rp);

  debugf("RA: set for '%s': %u achievements active (%u kept running), %u rejected, "
         "%u leaderboards active (%u kept running), %u rejected, rich presence %s",
         r->title ? r->title : "?", n, kept, rejected, lbs, lb_kept, lb_rejected,
         rp != RC_OK ? "rejected" : rp_on ? "on" : "none");
  return (int)n;
}

void ra_patch_rom_start(void) {
  ra_task_hardcore_block(RA_HC_BLOCK_ROM, true);
  mbedtls_sha256_init(&rom_sha);
  rom_hashing = mbedtls_sha256_starts(&rom_sha, 0) == 0;
}

void ra_patch_rom_data(const void *data, unsigned len) {
  if(rom_hashing && mbedtls_sha256_update(&rom_sha, data, len) != 0) rom_hashing = false;
}

void ra_patch_rom_end(void) {
  unsigned char sum[32];
  unsigned i;
  bool known = false;
  if(rom_hashing && mbedtls_sha256_finish(&rom_sha, sum) == 0)
    for(i = 0; i < sizeof(rom_known) / sizeof(rom_known[0]); i++)
      if(!memcmp(sum, rom_known[i], sizeof(sum))) known = true;
  mbedtls_sha256_free(&rom_sha);
  rom_hashing = false;
  if(known)
    debugf("RA: ROM image is the known file%s", !memcmp(sum, rom_known[1], sizeof(sum)) ? " without the 54xx" : "");
  else
    debugf("RA: ROM image unknown (SHA-256 %02x%02x%02x%02x...), softcore only", sum[0], sum[1], sum[2], sum[3]);
  ra_task_hardcore_block(RA_HC_BLOCK_ROM, !known);
}

void ra_patch_rom_gone(void) {
  if(rom_hashing) mbedtls_sha256_free(&rom_sha);
  rom_hashing = false;
  ra_task_hardcore_block(RA_HC_BLOCK_ROM, true);
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
  // only a set with a valid tag counts as the one the server sent last: its
  // fingerprint lets an unchanged server set pass, and hardcore may use it. An
  // untagged or edited one plays softcore until the server's set replaces it.
  if(card_verified) {
    fingerprint(r, card_fp);
    card_fp_valid = true;
    ra_task_hardcore_block(RA_HC_BLOCK_SET, false);
  } else
    debugf("RA: set on the card has no valid tag, softcore until the server's set arrives");
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
  // straight from the server over TLS: this set may count in hardcore
  ra_task_hardcore_block(RA_HC_BLOCK_SET, false);
  return 1;
}

void ra_patch_core_reset(void) { reset_due = true; }

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
  if(handover && xQueueReceive(handover, &r, 0) == pdTRUE) {
    activate_set(rt, r);
    discard(r);             // rcheevos has copied the conditions
    what |= RA_PATCH_NEW_SET;
  }
  return what;
}
