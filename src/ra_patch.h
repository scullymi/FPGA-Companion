/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.h
 *  @brief The current game and its achievement set, read from the card, see ra_patch.c. */
#ifndef RA_PATCH_H
#define RA_PATCH_H

#include <stdbool.h>
#include "rc_runtime.h"

/** @brief Hash the server knows the current game by, an md5 in hex. */
const char *ra_game_hash(void);
/** @brief Id of the current game on the server. */
unsigned    ra_game_id(void);

/** @brief A DIP switch the set expects at one value, by the switch's id in the core's XML. */
typedef struct {
  char id;      /**< the id of the list in the core's XML, e.g. 'L' */
  int  value;   /**< the listentry value the set expects, as the menu keeps it and the core gets it */
} ra_dip_t;

/** @brief The DIP switches the set expects, n of them. NULL when it expects none. */
const ra_dip_t *ra_game_dips(unsigned *n);

/** @brief Sets up the handover of a parsed set to com_task. Once, before the RA task runs. False when it failed. */
bool ra_patch_init(void);

/** @brief Reads the set from the card, parses it and hands it to com_task. RA task only.
 *
 *  Returns the number of core achievements in the file, which com_task will try
 *  to activate, or -1 when there is no usable set. */
int ra_patch_read_card(void);

/** @brief Activates a set the RA task handed over, if one waits. com_task only, before each frame.
 *
 *  Achievements of the previous set that the new one does not carry are deactivated. */
void ra_patch_apply_pending(rc_runtime_t *rt);

/** @brief Number of core achievements in the active set. */
unsigned    ra_patch_count(void);
/** @brief 1-based position of an achievement id in the set, 0 when not in it. */
unsigned    ra_patch_index(unsigned id);
/** @brief Title of an achievement, "" when unknown. */
const char *ra_patch_title(unsigned id);

#endif /* RA_PATCH_H */
