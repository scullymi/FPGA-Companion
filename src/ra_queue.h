/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_queue.h
 *  @brief Unlocks on their way to the server, kept on the card until it confirms them, see ra_queue.c. */
#ifndef RA_QUEUE_H
#define RA_QUEUE_H

#include <stdbool.h>
#include <FreeRTOS.h>
#include <queue.h>

#define RA_QUEUE_HANDOVER 16   /**< unlocks com_task can hand over before the RA task takes them */

/** @brief One unlock waiting for the server. */
typedef struct {
  unsigned      id;     /**< achievement id */
  unsigned long when;   /**< unix time of the unlock, 0 when the clock was not set */
} ra_unlock_t;

/** @brief Creates the handover queue and returns it, for the RA task's queue set.
 *
 *  Once, before com_task can report an unlock. */
QueueHandle_t ra_queue_init(void);

/** @brief Hands an unlock over to the RA task. From com_task, never blocks. */
void ra_queue_add(unsigned id);

/** @brief Opens the queue for this account and counts its waiting unlocks. RA task only.
 *
 *  Every line on the card names the account it was earned with. Lines of another
 *  account are never sent, they are set aside. */
void ra_queue_open(const char *user);

/** @brief Takes one unlock from the handover queue and writes it to the card.
 *
 *  RA task only, once each time its queue set names the handover queue. With
 *  keep false (no account) the unlock is dropped. When the card fails, it is kept
 *  in RAM and written again with the next one. */
void ra_queue_take(bool keep);

/** @brief The next unlock to send. RA task only.
 *
 *  1 found, 0 none waiting, -1 the card cannot be read. */
int ra_queue_head(ra_unlock_t *u);

/** @brief The server has the unlock ra_queue_head() returned, mark it done. RA task only.
 *
 *  false when the card could not be updated, it is then sent again, which is harmless. */
bool ra_queue_pop(void);

/** @brief The server refuses that unlock for good, move it to the parked file. RA task only.
 *
 *  false when the card could not be updated, it then stays. */
bool ra_queue_park(void);

/** @brief The server refused that unlock but it may pass later, move it behind the others. RA task only.
 *
 *  false when the card could not be updated, it then stays in front. */
bool ra_queue_requeue(void);

/** @brief Number of this account's unlocks waiting for the server. */
unsigned ra_queue_pending(void);

#endif /* RA_QUEUE_H */
