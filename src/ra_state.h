/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_state.h
 *  @brief What the account has already unlocked, from the server and kept on the card, see ra_state.c. */
#ifndef RA_STATE_H
#define RA_STATE_H

#include <stdbool.h>
#include <stdint.h>

#define RA_STATE_MAX 128  /**< ids kept per list, hardcore and softcore each, as many as a set holds (RA_PATCH_MAX) */

/** @brief Reads the state of this account and game from the card. RA task only, once after the game is known.
 *
 *  Does nothing but log while ra_game_id() is 0. Replaces both lists. */
void ra_state_load(const char *user);

/** @brief Replaces one list with the server's. RA task only.
 *
 *  hardcore true for the hardcore list, false for the softcore list. Ids of the
 *  server's own pseudo achievements are left out. True when the list changed. */
bool ra_state_replace(bool hardcore, const uint32_t *ids, unsigned n);

/** @brief Writes both lists to the card. RA task only, after both were replaced. */
void ra_state_save(void);

/** @brief Counts an unlock as unlocked in its mode, e.g. once it is queued. RA task only.
 *
 *  hardcore true adds it to the hardcore list and takes it out of the softcore
 *  one. false adds it to the softcore list, unless the account has it in hardcore. */
void ra_state_add(unsigned id, bool hardcore);

/** @brief True when the account has this achievement in hardcore, or it is on its way there. Any task. */
bool ra_state_known(unsigned id);

/** @brief True when the account has it in softcore only. Any task. */
bool ra_state_softcore_only(unsigned id);

/** @brief Achievements of the active set unlocked in hardcore, or queued for it. */
unsigned ra_state_count(void);

/** @brief Achievements of the active set unlocked in softcore only. */
unsigned ra_state_softcore_count(void);

#endif /* RA_STATE_H */
