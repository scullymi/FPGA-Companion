/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_queue.c
 *  @brief Unlocks on their way to the server.
 *
 *  com_task only puts an unlock into a FreeRTOS queue, which never blocks the game
 *  loop. The RA task writes it to the card at once, as a line
 *  "id unixtime user mode hash tag": mode h for hardcore and s for softcore, hash
 *  the 32 hex md5 of the game the unlock was earned under (what the award request
 *  needs, and a line keeps its game across boots into other games), tag the
 *  HMAC-SHA256 of everything before it with the device key (ra_mac.c, label
 *  RA_QUEUE_LABEL). The mode counts only with a tag that checks out, so an edited
 *  card cannot make an unlock hardcore, and the hash is inside the tagged text, so
 *  an edited game fails the tag like an edited mode. Without a device key the
 *  line ends after the hash and counts as softcore when it is read back.
 *  The token after the mode must be the hash, 32 lowercase hex. A line without
 *  one, or with a token of another shape there, is malformed: it is marked done
 *  and skipped like a torn one, never sent. A line whose tag does not check out
 *  is set aside to the parked file and never sent.
 *  Without a device key, tagged lines wait. A line is the running game's when it
 *  carries its hash, see ra_queue_own(). Firmware that reads 127 characters per
 *  line splits a line with a hash into a bad fragment and a garbage fragment:
 *  going back to it loses what is still queued.
 *  Once the server has it, the line is marked done in place: its first digit
 *  becomes '#'. That is a single sector, nothing is rewritten or renamed, so a
 *  power cut can at worst leave a line unmarked, and sending an unlock twice is
 *  harmless, the server answers "User already has". When every line is done the
 *  file is deleted. Only the RA task touches the files. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <ff.h>
#include "debug.h"
#include "sdc.h"
#include "ra_task.h"
#include "ra_patch.h"   // ra_game_hash(): the running game, stamped into every new unlock
#include "ra_state.h"
#include "ra_queue.h"
#include "ra_mac.h"

#define RA_QUEUE_FILE     "/sd/ra_pending.txt"   /**< one line "id unixtime user mode hash tag" per unlock, '#' in front once done */
#define RA_QUEUE_PARKED   "/sd/ra_parked.txt"    /**< unlocks that are not sent: refused for good, or of another account */
#define RA_QUEUE_LINE_MAX 160                    /**< 10+1+10+1+31+1+1+1+32+1+64+1 = 154 characters at most, and the NUL */
#define RA_QUEUE_LABEL    "g20k-q2"              /**< what the tag of a queue line is made over, keeps it apart from other tags */
#define RA_QUEUE_USER_MAX 32                     /**< longest user name kept */
#define RA_QUEUE_RAM      8                      /**< unlocks kept in RAM while the card fails */
#define RA_QUEUE_DONE     '#'                    /**< written over the first digit of a line that is done */

/** What the first line not yet done is. */
enum { LINE_NONE, LINE_OWN, LINE_OTHER, LINE_BAD, LINE_FORGED };

/** How a line parses: torn, fine, a tag that does not check out, a malformed token. */
enum { PARSE_BAD, PARSE_OK, PARSE_FORGED, PARSE_JUNK };

static QueueHandle_t handover;                  // com_task -> RA task
// static storage for it, like the leaderboard queue in ra_task.c: the FreeRTOS
// heap is nearly full once FTP runs
static StaticQueue_t handover_ctl;
static uint8_t       handover_mem[RA_QUEUE_HANDOVER * sizeof(ra_unlock_t)];
static char          owner[RA_QUEUE_USER_MAX];  // the account unlocks are kept for
static unsigned      pending;                   // this account's lines not done, plus ram[]
static ra_unlock_t   ram[RA_QUEUE_RAM];         // unlocks the card did not take
static volatile unsigned ram_n;                 // volatile: ra_queue_in_transit() reads it from com_task
static volatile bool taking;                    // ra_queue_take() holds an unlock between the queue and the card
static ra_unlock_t   head;                      // what ra_queue_head() returned last
static bool          head_in_ram;               // ... it is ram[0]
static bool          head_taken;                // ... and a submit is using it
static FSIZE_t       head_at;                   // file offset of its line
static char          head_line[RA_QUEUE_LINE_MAX];
static char          head_user[RA_QUEUE_USER_MAX];

unsigned ra_queue_in_transit(void) {
  // in this order: an unlock leaves the queue only after taking is set, and taking
  // is cleared only once the unlock is on the card or in ram[]
  unsigned n = handover ? (unsigned)uxQueueMessagesWaiting(handover) : 0;
  if(taking) n++;
  return n + ram_n;
}

QueueHandle_t ra_queue_init(void) {
  if(!handover) handover = xQueueCreateStatic(RA_QUEUE_HANDOVER, sizeof(ra_unlock_t), handover_mem, &handover_ctl);
  return handover;
}

void ra_queue_add(unsigned id, bool hardcore) {
  ra_unlock_t u = { id, (unsigned long)time(NULL), hardcore, "" };
  if(u.when < RA_CLOCK_VALID) u.when = 0;
  // the running game: the only one that can produce an unlock this boot. Not while
  // another game's ROM is in the core, its set is off and a trigger from the RAM
  // of that ROM is no unlock of this game
  if(ra_patch_foreign_rom()) {
    debugf("RA: unlock %u dropped, the ROM in the core is not the game's", id);
    return;
  }
  snprintf(u.hash, sizeof(u.hash), "%s", ra_game_hash());
  // a set is active only with an identity, so the hash is there by construction.
  // A line without one would read back as malformed
  if(!u.hash[0]) {
    debugf("RA: unlock %u without a game, dropped", id);
    return;
  }
  if(!handover || xQueueSend(handover, &u, 0) != pdTRUE)
    debugf("RA: unlock %u lost, the RA task does not take it", id);
}

bool ra_queue_own(const char *hash) {
  const ra_game_t *g;
  if(!strcmp(hash, ra_game_hash())) return true;
  // the hash of the table game the server resolved a fallback identity to (a
  // clone set's name): ra_patch_resolved() takes such an id only on the game's
  // own board. The reverse, a line stamped with a fallback hash and read back
  // under the table game, stays foreign: only the server knows that link
  g = ra_games_by_hash(hash);
  return g && ra_game_id() && g->id == ra_game_id();
}

/* Appends one line. If a power cut left the last line without its newline, the
   newline comes first, so the new line never runs into it. Every result counts,
   f_close() included, it writes the last sector. */
static FRESULT append_line(const char *path, const char *text) {
  FIL f;
  char last = '\n';
  UINT n;

  sdc_lock();
  FRESULT r = f_open(&f, path, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
  if(r == FR_OK) {
    FSIZE_t size = f_size(&f);
    if(size) {
      r = f_lseek(&f, size - 1);
      if(r == FR_OK) r = f_read(&f, &last, 1, &n);
    }
    if(r == FR_OK) r = f_lseek(&f, size);
    if(r == FR_OK && last != '\n' && f_putc('\n', &f) < 0) r = FR_DISK_ERR;
    if(r == FR_OK && f_puts(text, &f) < 0) r = FR_DISK_ERR;
    FRESULT c = f_close(&f);
    if(r == FR_OK) r = c;
  }
  sdc_unlock();
  return r;
}

/* Writes u as a line with its own hash: a line moved back or parked keeps the
   game it was earned under, never the running one. */
static FRESULT append_unlock(const char *path, const ra_unlock_t *u) {
  char line[RA_QUEUE_LINE_MAX], tag[RA_MAC_HEX + 1];
  int n = snprintf(line, sizeof(line), "%u %lu %s %c %s", u->id, u->when, owner, u->hardcore ? 'h' : 's', u->hash);
  // the tag over everything before it, the hash included. Without a device key
  // the line has none and counts as softcore when it is read back.
  if(n > 0 && n < (int)sizeof(line) && ra_mac_tag(RA_QUEUE_LABEL, line, (size_t)n, tag))
    snprintf(line + n, sizeof(line) - (size_t)n, " %s\n", tag);
  else
    snprintf(line + n, sizeof(line) - (size_t)n, "\n");
  return append_line(path, line);
}

/* Writes RA_QUEUE_DONE over the first character of the line at this offset. */
static FRESULT mark_done(FSIZE_t at) {
  FIL f;
  UINT n = 0;

  sdc_lock();
  FRESULT r = f_open(&f, RA_QUEUE_FILE, FA_WRITE);
  if(r == FR_OK) {
    r = f_lseek(&f, at);
    if(r == FR_OK) r = f_write(&f, "#", 1, &n);
    if(r == FR_OK && n != 1) r = FR_DISK_ERR;
    FRESULT c = f_close(&f);
    if(r == FR_OK) r = c;
  }
  sdc_unlock();
  return r;
}

/* "id unixtime user mode hash tag" with its newline, mode and tag may be
   missing. A line a power cut left without its end, or anything else, is
   PARSE_BAD. The token after the mode must be the hash, 32 lowercase hex, then
   none or a tag under RA_QUEUE_LABEL. Any other token there, or none, is
   PARSE_JUNK: the line is malformed, nothing waits for it. The mode counts only
   with a tag that checks out. Without a tag the line is softcore. A tag that does
   not check out, or one that cannot be checked because there is no device key,
   makes it PARSE_FORGED. A 64-character tag edited down to 32 characters looks
   like a hash: the line then reads as untagged with that hash, softcore, and the
   server refuses it until it is parked after RA_REFUSE_MAX refusals. */
static int parse(const char *line, ra_unlock_t *u, char *user) {
  size_t len = strlen(line);
  if(len < 2 || line[len - 1] != '\n' || line[0] < '0' || line[0] > '9') return PARSE_BAD;
  char *end;
  u->id   = (unsigned)strtoul(line, &end, 10);   // strtoul, sscanf would pull about 47 KB of newlib in
  u->when = strtoul(end, &end, 10);
  while(*end == ' ') end++;
  size_t n = strcspn(end, " \r\n");
  if(!u->id || !n || n >= RA_QUEUE_USER_MAX) return PARSE_BAD;
  memcpy(user, end, n);
  user[n] = 0;
  // the mode letter after the user
  end += n;
  while(*end == ' ') end++;
  char mode = (*end == 'h' || *end == 's') && (end[1] == ' ' || end[1] == '\r' || end[1] == '\n') ? *end : 0;
  if(mode) end++;
  while(*end == ' ') end++;
  u->hardcore = false;
  // the hash
  n = strcspn(end, " \r\n");
  if(n != RA_GAMES_HASH_LEN) return PARSE_JUNK;
  for(size_t i = 0; i < n; i++)
    if(!((end[i] >= '0' && end[i] <= '9') || (end[i] >= 'a' && end[i] <= 'f'))) return PARSE_JUNK;
  memcpy(u->hash, end, n);
  u->hash[n] = 0;
  end += n;
  while(*end == ' ') end++;
  n = strcspn(end, " \r\n");
  if(*end == '\r' || *end == '\n') return PARSE_OK;   // no tag: softcore
  // the tag, over the text before the space in front of it
  size_t text = (size_t)(end - line);
  while(text && line[text - 1] == ' ') text--;
  char tag[RA_MAC_HEX + 1];
  if(n != RA_MAC_HEX) return PARSE_JUNK;
  memcpy(tag, end, RA_MAC_HEX);
  tag[RA_MAC_HEX] = 0;
  if(!ra_mac_check(RA_QUEUE_LABEL, line, text, tag)) return PARSE_FORGED;
  u->hardcore = mode == 'h';
  return PARSE_OK;
}

/* Reads the file. The first line not done goes to head, head_at, head_line and
   head_user, and its kind is returned, -1 on a card error. This account's lines
   are counted on the way, those of the running game also count as unlocked: the
   state lists are that game's. A malformed line counts as unreadable (LINE_BAD),
   marked done on its turn. A blank line is skipped and never marked: the '#'
   would land on its newline and join it with the next line. A file with only done
   and blank lines is deleted, that cannot lose anything. */
static int scan(void) {
  FIL f;
  char line[RA_QUEUE_LINE_MAX], user[RA_QUEUE_USER_MAX];
  int kind = LINE_NONE;
  unsigned own = 0;
  bool lines = false;

  sdc_lock();
  FRESULT r = f_open(&f, RA_QUEUE_FILE, FA_READ);
  if(r == FR_OK) {
    for(;;) {
      FSIZE_t at = f_tell(&f);
      if(!f_gets(line, sizeof(line), &f)) break;
      lines = true;
      if(line[0] == RA_QUEUE_DONE || line[0] == '\n' || line[0] == '\r') continue;
      ra_unlock_t u = { 0, 0, false, "" };
      int p = parse(line, &u, user);
      int k = p == PARSE_BAD || p == PARSE_JUNK ? LINE_BAD : p == PARSE_FORGED ? LINE_FORGED :
              strcasecmp(user, owner) ? LINE_OTHER : LINE_OWN;
      // every line to send counts as pending; queued counts as unlocked, in the
      // mode it was earned in, when it is the running game's
      if(k == LINE_OWN) {
        own++;
        if(ra_queue_own(u.hash)) ra_state_add(u.id, u.hardcore);
      }
      if(kind == LINE_NONE) {
        kind = k; head = u; head_at = at;
        snprintf(head_line, sizeof(head_line), "%s", line);
        snprintf(head_user, sizeof(head_user), "%s", k == LINE_OWN || k == LINE_OTHER ? user : "");
      }
    }
    if(f_error(&f)) r = FR_DISK_ERR;
    f_close(&f);
    if(r == FR_OK && lines && kind == LINE_NONE) f_unlink(RA_QUEUE_FILE);
  }
  sdc_unlock();

  if(r == FR_NO_FILE) r = FR_OK;
  if(r != FR_OK) return -1;
  pending = own + ram_n;
  return kind;
}

/* Unlocks the card did not take are written again, oldest first, once it works.
   The one a submit is using stays where it is. */
static void flush_ram(void) {
  unsigned first = (head_taken && head_in_ram) ? 1 : 0;
  while(ram_n > first) {
    if(append_unlock(RA_QUEUE_FILE, &ram[first]) != FR_OK) return;
    debugf("RA: unlock %u from RAM now on the card", ram[first].id);
    ram_n--;
    memmove(ram + first, ram + first + 1, (ram_n - first) * sizeof(ram[0]));
  }
}

void ra_queue_open(const char *user) {
  snprintf(owner, sizeof(owner), "%s", user);
  if(scan() < 0)
    debugf("RA: %s not readable", RA_QUEUE_FILE);
  else if(pending)
    debugf("RA: %u unlocks pending from before", pending);
}

static void take_one(bool keep) {
  ra_unlock_t u;
  if(!handover || xQueueReceive(handover, &u, 0) != pdTRUE) return;
  if(!keep) {
    debugf("RA: unlock %u not kept, no account", u.id);
    return;
  }
  // what the account already has in this mode, or in hardcore, is not sent again
  if(ra_state_known(u.id) || (!u.hardcore && ra_state_softcore_only(u.id))) {
    debugf("RA: unlock %u already unlocked, not queued", u.id);
    return;
  }
  flush_ram();
  FRESULT r = append_unlock(RA_QUEUE_FILE, &u);
  if(r == FR_OK) {
    ra_state_add(u.id, u.hardcore);   // on the card, so from now on it counts as unlocked
    pending++;
    debugf("RA: %s unlock %u queued (%u pending)%s", u.hardcore ? "hardcore" : "softcore", u.id,
           pending, u.when ? "" : ", clock not set, unlock time unknown");
  } else if(ram_n < RA_QUEUE_RAM) {
    ram[ram_n++] = u;
    pending++;
    debugf("RA: unlock %u NOT on the card (error %d), kept in RAM until it can be written", u.id, (int)r);
  } else
    debugf("RA: unlock %u lost, card error %d and no room in RAM", u.id, (int)r);
}

void ra_queue_take(bool keep) {
  taking = true;   // counted by ra_queue_in_transit() from before the receive
  take_one(keep);
  taking = false;
}

int ra_queue_head(ra_unlock_t *u) {
  head_taken = false;
  flush_ram();
  head_in_ram = ram_n > 0;
  if(head_in_ram) {
    head = ram[0];
  } else {
    int k;
    // what cannot be sent is set aside first: another account's line, and one whose
    // tag does not check out, go to the parked file, an unreadable or malformed
    // one is only marked done
    while((k = scan()) == LINE_OTHER || k == LINE_BAD || k == LINE_FORGED) {
      // without a device key a tag cannot be checked: the line waits instead of
      // being set aside, a key may come back
      if(k == LINE_FORGED && !ra_mac_ready()) {
        static bool told;
        if(!told) debugf("RA: tagged unlocks wait, there is no device key to check them");
        told = true;
        return 0;
      }
      if(k == LINE_OTHER) {
        debugf("RA: unlock %u of account '%s' set aside to %s", head.id, head_user, RA_QUEUE_PARKED);
        if(append_line(RA_QUEUE_PARKED, head_line) != FR_OK) return -1;
      } else if(k == LINE_FORGED) {
        debugf("RA: unlock %u %s, set aside to %s, not sent", head.id,
               ra_mac_ready() ? "with a tag that does not check out" : "with a tag, but no device key to check it",
               RA_QUEUE_PARKED);
        if(append_line(RA_QUEUE_PARKED, head_line) != FR_OK) return -1;
      } else
        debugf("RA: unreadable or malformed line in %s marked done, skipped", RA_QUEUE_FILE);
      if(mark_done(head_at) != FR_OK) return -1;
    }
    if(k != LINE_OWN) return k;               // LINE_NONE is 0, a card error -1
  }
  head_taken = true;
  *u = head;
  return 1;
}

bool ra_queue_pop(void) {
  if(!head_taken) return false;
  if(head_in_ram) {
    ram_n--;
    memmove(ram, ram + 1, ram_n * sizeof(ram[0]));
  } else {
    FRESULT r = mark_done(head_at);
    if(r != FR_OK) {
      debugf("RA: unlock %u not marked done (error %d), it is sent again", head.id, (int)r);
      return false;
    }
  }
  head_taken = false;
  if(pending) pending--;
  return true;
}

bool ra_queue_park(void) {
  if(!head_taken) return false;
  FRESULT r = append_unlock(RA_QUEUE_PARKED, &head);
  if(r != FR_OK) {
    debugf("RA: unlock %u could not be parked (error %d)", head.id, (int)r);
    return false;
  }
  return ra_queue_pop();
}

bool ra_queue_requeue(void) {
  if(!head_taken) return false;
  if(head_in_ram) {
    ra_unlock_t u = ram[0];
    memmove(ram, ram + 1, (ram_n - 1) * sizeof(ram[0]));
    ram[ram_n - 1] = u;
  } else {
    // a copy at the end first: a power cut in between only leaves it twice
    FRESULT r = append_unlock(RA_QUEUE_FILE, &head);
    if(r == FR_OK) r = mark_done(head_at);
    if(r != FR_OK) {
      debugf("RA: unlock %u could not be moved back (error %d)", head.id, (int)r);
      return false;
    }
  }
  head_taken = false;
  return true;
}

unsigned ra_queue_pending(void) { return pending; }
