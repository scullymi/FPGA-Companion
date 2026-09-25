/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/* The achievement set of the current game, read from the card. See ra_patch.c. */
#ifndef RA_PATCH_H
#define RA_PATCH_H

#include "rc_runtime.h"

/* Galaga on RetroAchievements */
#define RA_PATCH_GAME_ID 12138u

/* Once, after rc_runtime_init(): reads the set from the card and activates it.
   Returns the number of achievements, or -1 when there is no usable set. */
int ra_patch_load(rc_runtime_t *rt);

/* The active set: how many core achievements, the 1-based position of an id in
   it (0 = not in the set) and its title ("" when unknown). */
unsigned    ra_patch_count(void);
unsigned    ra_patch_index(unsigned id);
const char *ra_patch_title(unsigned id);

#endif /* RA_PATCH_H */
