/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.h
 *  @brief The achievement set of the current game, read from the card, see ra_patch.c. */
#ifndef RA_PATCH_H
#define RA_PATCH_H

#include "rc_runtime.h"

/* Galaga on RetroAchievements, and the hash the server knows it by: md5("galaga"),
   the name of the arcade ROM set */
#define RA_PATCH_GAME_ID   12138u                               /**< game id on the server */
#define RA_PATCH_GAME_HASH "b8140b5e33c53b0f7dd3cc368951a4dd"   /**< md5 of the ROM set name */

/** @brief Reads the set from the card and activates it. Once, after rc_runtime_init().
 *
 *  Returns the number of achievements, or -1 when there is no usable set. */
int ra_patch_load(rc_runtime_t *rt);

/** @brief Number of core achievements in the active set. */
unsigned    ra_patch_count(void);
/** @brief 1-based position of an achievement id in the set, 0 when not in it. */
unsigned    ra_patch_index(unsigned id);
/** @brief Title of an achievement, "" when unknown. */
const char *ra_patch_title(unsigned id);

#endif /* RA_PATCH_H */
