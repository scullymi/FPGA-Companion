/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.h
 *  @brief The current game and its achievement set, read from the card, see ra_patch.c. */
#ifndef RA_PATCH_H
#define RA_PATCH_H

#include "rc_runtime.h"

/** @brief Hash the server knows the current game by, an md5 in hex. */
const char *ra_game_hash(void);
/** @brief Id of the current game on the server. */
unsigned    ra_game_id(void);

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
