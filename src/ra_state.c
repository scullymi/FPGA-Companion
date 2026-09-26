/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_state.c
 *  @brief What the account has already unlocked.
 *
 *  Two lists of ids: unlocked in hardcore, and unlocked in softcore only, which
 *  is the server's softcore list without the hardcore one. They come from
 *  r=unlocks after the login and are
 *  kept on the card, so they also hold offline. An unlock the account has in
 *  hardcore is not sent again. One it has only in softcore is, so that a
 *  hardcore submission upgrades it. Only the RA task changes the lists,
 *  com_task only reads them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <FreeRTOS.h>
#include <task.h>
#include <ff.h>
#include "debug.h"
#include "sdc.h"
#include "ra_patch.h"
#include "ra_state.h"

#define RA_STATE_FILE     "/sd/ra_unlocked.txt"   /**< "# game <id> <user>", then one id per line, an s in front of softcore only ones */
#define RA_STATE_LINE_MAX 64                      /**< the header line is the longest */
#define RA_STATE_REAL_ID  100000000u              /**< the server's own pseudo achievements have ids from here on */

// the two lists and the account they belong to
static unsigned hard[RA_STATE_MAX], hard_n;   // unlocked in hardcore, or queued for it
static unsigned soft[RA_STATE_MAX], soft_n;   // unlocked in softcore only
static char     owner[32];

static bool in_list(const unsigned *list, unsigned n, unsigned id) {
  for(unsigned i = 0; i < n; i++) if(list[i] == id) return true;
  return false;
}

// com_task reads the lists while the RA task may change them. Every change
// after the start runs with interrupts off, so on this single core a reader
// never sees a list half copied or a count ahead of its entries.
bool     ra_state_known(unsigned id)         { return in_list(hard, hard_n, id); }
bool     ra_state_softcore_only(unsigned id) { return in_list(soft, soft_n, id); }
unsigned ra_state_count(void)                { return hard_n; }
unsigned ra_state_softcore_count(void)       { return soft_n; }

// takes id out of soft, if it is there. Under the critical section of the caller.
static void drop_soft(unsigned id) {
  for(unsigned i = 0; i < soft_n; i++)
    if(soft[i] == id) { soft[i] = soft[--soft_n]; return; }
}

void ra_state_add(unsigned id) {
  if(ra_state_known(id) || hard_n >= RA_STATE_MAX) return;
  taskENTER_CRITICAL();
  hard[hard_n++] = id;
  drop_soft(id);            // it is on its way to hardcore, so no longer softcore only
  taskEXIT_CRITICAL();
}

void ra_state_replace(bool hardcore, const uint32_t *ids, unsigned n) {
  unsigned tmp[RA_STATE_MAX], m = 0, fresh = 0, i;

  // the new list, without the server's pseudo achievements. For the softcore
  // list also without what is in hardcore, the server lists those in both.
  for(i = 0; i < n && m < RA_STATE_MAX; i++) {
    unsigned id = (unsigned)ids[i];
    if(id >= RA_STATE_REAL_ID) continue;
    if(!hardcore && in_list(hard, hard_n, id)) continue;
    if(!in_list(hardcore ? hard : soft, hardcore ? hard_n : soft_n, id)) fresh++;
    tmp[m++] = id;
  }
  if(i < n) debugf("RA: more than %u unlocks, the rest is ignored", (unsigned)RA_STATE_MAX);

  taskENTER_CRITICAL();
  if(hardcore) {
    memcpy(hard, tmp, m * sizeof(hard[0]));
    hard_n = m;
    for(unsigned i = 0; i < m; i++) drop_soft(tmp[i]);   // hardcore now, so no longer softcore only
  } else {
    memcpy(soft, tmp, m * sizeof(soft[0]));
    soft_n = m;
  }
  taskEXIT_CRITICAL();

  if(hardcore) debugf("RA: server: %u unlocked in hardcore (%u new here)", m, fresh);
  else         debugf("RA: server: %u unlocked in softcore only (%u new here)", m, fresh);
}

void ra_state_save(void) {
  FIL f;
  char line[RA_STATE_LINE_MAX];
  FRESULT r;
  bool ok = true;

  // the whole file anew. A power cut in between leaves a file without a full
  // header, which ra_state_load() ignores, and the server fills it again.
  sdc_lock();
  r = f_open(&f, RA_STATE_FILE, FA_WRITE | FA_CREATE_ALWAYS);
  if(r == FR_OK) {
    snprintf(line, sizeof(line), "# game %u %s\n", ra_game_id(), owner);
    ok = f_puts(line, &f) >= 0;
    for(unsigned i = 0; ok && i < hard_n; i++) {
      snprintf(line, sizeof(line), "%u\n", hard[i]);
      ok = f_puts(line, &f) >= 0;
    }
    for(unsigned i = 0; ok && i < soft_n; i++) {
      snprintf(line, sizeof(line), "s%u\n", soft[i]);
      ok = f_puts(line, &f) >= 0;
    }
    r = f_close(&f);
  }
  sdc_unlock();
  if(r != FR_OK || !ok) debugf("RA: %s not written (error %d)", RA_STATE_FILE, (int)r);
}

void ra_state_load(const char *user) {
  FIL f;
  char line[RA_STATE_LINE_MAX];
  unsigned h = 0, s = 0;

  snprintf(owner, sizeof(owner), "%s", user);

  sdc_lock();
  if(f_open(&f, RA_STATE_FILE, FA_READ) == FR_OK) {
    // the header names game and account, a file of another one is ignored:
    // its ids would mean nothing here
    unsigned long game = 0;
    char who[32] = "";
    bool header = f_gets(line, sizeof(line), &f) && strncmp(line, "# game ", 7) == 0;
    if(header) {
      char *end;
      game = strtoul(line + 7, &end, 10);
      while(*end == ' ') end++;
      snprintf(who, sizeof(who), "%.*s", (int)strcspn(end, " \r\n"), end);
    }
    if(!header)
      debugf("RA: %s has no header, ignored", RA_STATE_FILE);   // e.g. cut short by a power cut
    else if(game == ra_game_id() && !strcasecmp(who, owner)) {
      // one id per line, an s in front means softcore only. No critical
      // section: the counts are stored last, so a reader sees empty lists
      // until every entry is in place
      while(f_gets(line, sizeof(line), &f)) {
        bool sc = line[0] == 's';
        unsigned id = (unsigned)strtoul(line + (sc ? 1 : 0), NULL, 10);
        if(!id || id >= RA_STATE_REAL_ID) continue;
        if(!sc && h < RA_STATE_MAX) hard[h++] = id;
        if(sc  && s < RA_STATE_MAX) soft[s++] = id;
      }
    } else
      debugf("RA: %s is for another game or account, ignored", RA_STATE_FILE);
    f_close(&f);
  }
  sdc_unlock();

  hard_n = h;
  soft_n = s;
  if(h || s) debugf("RA: %u unlocked according to the card, %u only in softcore", h, s);
}
