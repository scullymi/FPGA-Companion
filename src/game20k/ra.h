/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra.h
 *  @brief What the rest of the firmware calls of RetroAchievements.
 *
 *  With GAME20K_RA 1 these are the real headers. With 0 each call does nothing, and hardcore is
 *  never on. GAME20K_RA must be set to one of them: a source that missed it would silently take
 *  the empty calls, the write protection of FTP among them. */
#ifndef RA_H
#define RA_H

#ifndef GAME20K_RA
#error "GAME20K_RA must be 0 or 1, see game20k.cmake"
#endif

#if GAME20K_RA
#include "ra_events.h"
#include "ra_mac.h"
#include "ra_patch.h"
#include "ra_queue.h"
#include "ra_task.h"
#else
#include <stdbool.h>

#define RA_HC_BLOCK_XML  0u
#define RA_HC_BLOCK_MENU 0u

static inline bool ra_patch_init(void) { return true; }
static inline void ra_patch_core_reset(void) {}
static inline void ra_mac_init(void) {}
static inline void ra_task_start(void) {}
static inline void ra_task_clock_set(void) {}
static inline void ra_task_core_flags(unsigned char flags) { (void)flags; }
static inline void ra_task_core_value(char id, int value) {
  (void)id;
  (void)value;
}
static inline void ra_task_hardcore_block(unsigned bits, bool on) {
  (void)bits;
  (void)on;
}
static inline bool ra_task_hardcore(void) { return false; }
static inline unsigned ra_task_lboard_pending(void) { return 0; }
static inline unsigned ra_queue_in_transit(void) { return 0; }
static inline void ra_events_header(unsigned char *hdr_tx) { (void)hdr_tx; }
static inline void ra_events_prepare(void) {}
static inline void ra_events_frame(const unsigned char *ram, unsigned size) {
  (void)ram;
  (void)size;
}
#endif

#endif /* RA_H */
