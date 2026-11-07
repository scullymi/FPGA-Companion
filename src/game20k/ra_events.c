/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_events.c
 *  @brief rcheevos over each good snapshot of the game RAM, and what its events cause. */
#include <malloc.h> /* mallinfo(), the SDK heap's top after a game switch */
#include <stdio.h>
#include <string.h>

#include "../mcu_hw.h"
#include "../debug.h"
#include "pico/time.h"
#include "rc_runtime.h"
#include "banner.h"
#include "ra_events.h"
#include "ra_patch.h"
#include "ra_queue.h"
#include "ra_state.h"
#include "ra_task.h"

uint32_t getFreeHeap(void);  /* mcu_hw.c: the SDK heap left for mbedTLS and rcheevos */
uint32_t getTotalHeap(void); /* mcu_hw.c: the SDK heap, all of it */

/* rcheevos evaluates the achievement conditions over each good snapshot. The set
   is defined on the flat layout the core sends, per game (Galaga: bgram 2048, then
   wram1..3 of 1024 each), so an address is the offset in the snapshot. */
static rc_runtime_t ra_rt;
static bool ra_ready;
static unsigned ra_gen;             /* the game generation ra_rt is made for, game_generation() */
static unsigned char ra_triggered;  /* achievements triggered, saturating */
static unsigned char ra_last;       /* last one, 1-based position in the set */
static unsigned short ra_us;        /* evaluation time of the last snapshot */
static unsigned ra_oob;             /* reads outside the mirror */
static unsigned ra_rp_frames;       /* frames since the rich presence text was read */
#define RA_RP_EVERY 60              /* frames between two readings, about a second */
static const unsigned char *ra_ram; /* the game RAM of the snapshot being evaluated */
static unsigned ra_ram_size;        /* and its size, the running core's */

/* rcheevos asks for num_bytes from address into buffer and gets back how many of them
   exist. Outside the running core's game RAM there is nothing: say so once instead of
   reading air, or stale bytes of a bigger game in the buffer. Compared without a sum,
   so that an address near 2^32 cannot wrap into range. */
static uint32_t ra_peek(uint32_t address, uint8_t *buffer, uint32_t num_bytes, void *ud) {
  (void)ud;
  if(address >= ra_ram_size) {
    if(!ra_oob++) debugf("RA: condition reads 0x%lx, outside the mirror", (unsigned long)address);
    return 0;
  }
  if(num_bytes > ra_ram_size - address) num_bytes = ra_ram_size - address;
  memcpy(buffer, ra_ram + address, num_bytes);
  return num_bytes;
}

/* Challenges that are on, one bit per position in the set's table. An
   achievement is primed when all its conditions but the trigger hold, e.g. a
   stage without losing a ship, and RetroAchievements asks that this shows during
   play: header byte 5 bit 0 lights a marker next to the picture. */
static uint32_t ra_primed[(RA_PATCH_MAX + 31) / 32];

static void ra_primed_set(unsigned id, bool on) {
  unsigned i = ra_patch_index(id);
  if(!i || i > RA_PATCH_MAX) return;
  i--;
  if(on) ra_primed[i / 32] |= (1u << (i % 32));
  else ra_primed[i / 32] &= ~(1u << (i % 32));
}

static bool ra_primed_any(void) {
  for(unsigned w = 0; w < sizeof(ra_primed) / sizeof(ra_primed[0]); w++)
    if(ra_primed[w]) return true;
  return false;
}

/* Unlocked in the mode that counts, those show neither challenge nor progress. */
static bool ra_done(unsigned id) {
  return ra_state_known(id) || (!ra_task_hardcore() && ra_state_softcore_only(id));
}

/* Leaderboards: an attempt starts, ends without a result, or is finished with a
   value. Only hardcore counts, the server keeps an entry as a hardcore one and
   only from a client it has approved: in softcore a result only goes to the
   log. A banner when an attempt starts and one with the result, no live
   tracker, the value is the score the game shows anyway. RetroAchievements
   allows leaderboard popups to be off, not the submission. */
static void ra_lboard_event(const rc_runtime_event_t *ev) {
  ra_patch_lboard_t lb;
  char text[64], value[24]; // banner_show() cuts to the banner, at a word where it can
  bool hardcore = ra_task_hardcore();
  if(!ra_patch_lboard(ev->id, &lb)) return;
  switch(ev->type) {
  case RC_RUNTIME_EVENT_LBOARD_STARTED:
    debugf("RA: leaderboard %u started: %s", (unsigned)ev->id, lb.title);
    if(hardcore) {
      snprintf(text, sizeof(text), "RA: LB %s", lb.title);
      banner_show(text, true, false);
    }
    break;
  case RC_RUNTIME_EVENT_LBOARD_CANCELED:
    debugf("RA: leaderboard %u attempt ended without a result", (unsigned)ev->id);
    break;
  case RC_RUNTIME_EVENT_LBOARD_TRIGGERED:
    rc_runtime_format_lboard_value(value, sizeof(value), ev->value, lb.format);
    if(!hardcore) {
      debugf("RA: leaderboard %u: %s, not submitted in softcore", (unsigned)ev->id, value);
      break;
    }
    debugf("RA: leaderboard %u: %s, goes to the server", (unsigned)ev->id, value);
    snprintf(text, sizeof(text), "RA: LB RESULT %s", value);
    banner_show(text, true, true);
    ra_task_lboard(ev->id, ev->value);
    break;
  default:
    break; // UPDATED comes with every change of the value, DISABLED needs address checks this firmware does not make
  }
}

static void ra_event(const rc_runtime_event_t *ev) {
  switch(ev->type) {
  case RC_RUNTIME_EVENT_LBOARD_STARTED:
  case RC_RUNTIME_EVENT_LBOARD_CANCELED:
  case RC_RUNTIME_EVENT_LBOARD_UPDATED:
  case RC_RUNTIME_EVENT_LBOARD_TRIGGERED:
  case RC_RUNTIME_EVENT_LBOARD_DISABLED:
    ra_lboard_event(ev);
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_PRIMED:
    if(!ra_done(ev->id)) ra_primed_set(ev->id, true);
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_UNPRIMED:
  case RC_RUNTIME_EVENT_ACHIEVEMENT_RESET:
  case RC_RUNTIME_EVENT_ACHIEVEMENT_PAUSED:
  case RC_RUNTIME_EVENT_ACHIEVEMENT_DISABLED:
    ra_primed_set(ev->id, false);
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_PROGRESS_UPDATED:
    if(!ra_done(ev->id)) {
      char progress[RA_PATCH_PROGRESS_MAX];
      ra_patch_format_progress(&ra_rt, ev->id, progress, sizeof(progress));
      if(progress[0]) banner_show_progress(progress, ra_patch_title(ev->id), ra_task_hardcore());
    }
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED:
    ra_primed_set(ev->id, false);
    break;
  default:
    return;
  }
  bool hardcore = ra_task_hardcore();
  if(ra_triggered < 255) ra_triggered++;
  ra_last = (unsigned char)ra_patch_index(ev->id);
  debugf("RA: %s achievement %u triggered: %s", hardcore ? "hardcore" : "softcore", (unsigned)ev->id,
         ra_patch_title(ev->id));
  // gold in hardcore, white in softcore, as on the RA site. The mark is green when
  // the unlock is new in this mode, grey when the account has it already or one
  // is queued. Asked before the queue takes this one.
  bool known = ra_state_known(ev->id) || (!hardcore && ra_state_softcore_only(ev->id));
  banner_show(ra_patch_title(ev->id), hardcore, !known);
  // the RA task keeps it on the card and sends it, this never blocks
  ra_queue_add(ev->id, hardcore);
}

/* game20k: the heaps after a game switch, once per switch: free in all and the
   largest piece, so ten switches show whether they cut the heap up. The SDK heap
   (rcheevos, mbedTLS, the parsed set) tells its largest piece only at the top, the
   chunk there and what was never taken from the system: a free chunk below may be
   larger, mallinfo() does not say. */
static void ra_heap_log(void) {
  struct mallinfo m = mallinfo();
  HeapStats_t h;
  vPortGetHeapStats(&h);
  debugf("RA: game switch %u, SDK heap free %lu, top %lu, %u free chunks, FreeRTOS heap free %u, largest %u",
         ra_gen, (unsigned long)getFreeHeap(), (unsigned long)(getTotalHeap() - m.arena + m.keepcost),
         (unsigned)m.ordblks, (unsigned)h.xAvailableHeapSpaceInBytes,
         (unsigned)h.xSizeOfLargestFreeBlockInBytes);
}

/* The back channel for the diagnostic bars of the FPGA: the rcheevos state. */
void ra_events_header(unsigned char *hdr_tx) {
  hdr_tx[0] = ra_triggered;
  hdr_tx[1] = (unsigned char)ra_patch_count();
  hdr_tx[2] = (unsigned char)(ra_us & 0xff);
  hdr_tx[3] = (unsigned char)(ra_us >> 8);
  hdr_tx[4] = ra_last;
  hdr_tx[5] = ra_primed_any() ? 0x01 : 0x00; /* bit 0: a challenge is on, the marker shows */
}

/* Before a snapshot that passed every check goes to rcheevos. */
void ra_events_prepare(void) {
  /* rcheevos only sees a snapshot that passed every check. The set comes from
     the RA task, which reads it from the card, and is activated here once it
     is there. game20k: another game of this board (game_new()) gets a
     new rcheevos before its first frame: memrefs are freed only with the runtime,
     and nothing of the old game's achievements, leaderboards, challenges or rich
     presence carries over. */
  if(ra_gen != game_generation()) {
    ra_gen = game_generation();
    if(ra_ready) rc_runtime_destroy(&ra_rt);
    ra_ready     = false;
    ra_triggered = 0;
    ra_last      = 0;
    ra_oob       = 0;
    ra_rp_frames = 0;
    memset(ra_primed, 0, sizeof(ra_primed));
    ra_task_set_richpresence(""); /* the old game's text never reaches a ping of the new one */
    ra_heap_log();
  }
  if(!ra_ready) {
    rc_runtime_init(&ra_rt);
    ra_ready = true;
  }
}

/* Evaluates one snapshot: ram is its game RAM of size bytes. */
void ra_events_frame(const unsigned char *ram, unsigned size) {
  ra_ram        = ram;
  ra_ram_size   = size;
  unsigned what = ra_patch_apply_pending(&ra_rt);
  /* the SDK heap is where rcheevos keeps the set and mbedTLS a connection: a large
     set leaves less for the next request, so the log shows what is left */
  if(what & RA_PATCH_NEW_SET)
    debugf("RA: set active, SDK heap free %lu, FreeRTOS heap free %u", (unsigned long)getFreeHeap(),
           (unsigned)xPortGetFreeHeapSize());
  if(what & RA_PATCH_RESET) { /* a game starts */
    ra_task_game_start();     /* hardcore due begins with this frame */
    banner_ask_mode();
  }
  if(what)
    memset(ra_primed, 0,
           sizeof(ra_primed)); /* after a reset nothing is primed, a new set moves the positions */
  absolute_time_t t0 = get_absolute_time();
  rc_runtime_do_frame(&ra_rt, ra_event, ra_peek, NULL, NULL);
  int64_t dt = absolute_time_diff_us(t0, get_absolute_time());
  ra_us      = dt > 65535 ? 65535 : (unsigned short)dt;

  /* the RA task pings only while frames arrive, and sends the rich presence text,
     read here where the snapshot is valid, about once a second */
  ra_task_frame();
  if(++ra_rp_frames >= RA_RP_EVERY) {
    char rp[RA_RP_MAX];
    ra_rp_frames = 0;
    rc_runtime_get_richpresence(&ra_rt, rp, sizeof(rp), ra_peek, NULL, NULL);
    ra_task_set_richpresence(rp);
    ra_patch_update_progress(&ra_rt); /* progress and challenges for the menu's list */
  }
}
