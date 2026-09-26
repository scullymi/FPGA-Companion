/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_state.h
 *  @brief What the account has already unlocked, from the server and kept on the card, see ra_state.c. */
#ifndef RA_STATE_H
#define RA_STATE_H

#include <stdbool.h>
#include <stdint.h>

#define RA_STATE_MAX 64   /**< ids kept per list, hardcore and softcore each */

/** @brief Reads the state of this account from the card. RA task only, once at its start. */
void ra_state_load(const char *user);

/** @brief Replaces one list with the server's. RA task only.
 *
 *  hardcore true for the hardcore list, false for the softcore list. Ids of the
 *  server's own pseudo achievements are left out. */
void ra_state_replace(bool hardcore, const uint32_t *ids, unsigned n);

/** @brief Writes both lists to the card. RA task only, after both were replaced. */
void ra_state_save(void);

/** @brief Counts an unlock as unlocked in hardcore, e.g. once it is queued. RA task only. */
void ra_state_add(unsigned id);

/** @brief True when the account has this achievement in hardcore, or it is on its way there. Any task. */
bool ra_state_known(unsigned id);

/** @brief True when the account has it in softcore only. Any task. */
bool ra_state_softcore_only(unsigned id);

/** @brief Number of achievements unlocked in hardcore. */
unsigned ra_state_count(void);

/** @brief Number of achievements unlocked in softcore only. */
unsigned ra_state_softcore_count(void);

#endif /* RA_STATE_H */
