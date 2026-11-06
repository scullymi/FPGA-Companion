/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file banner.h
 *  @brief The banner at the bottom of the picture, see banner.c. */
#ifndef BANNER_H
#define BANNER_H

#include <stdbool.h>

/** @brief One banner in the queue. */
typedef struct banner banner_t;

/** @brief Queues a banner, returns its place in the queue, NULL when the queue is full. */
banner_t *banner_show(const char *text, bool gold, bool new);

/** @brief Queues a banner about the device or the account, a game switch keeps it. */
banner_t *banner_show_kept(const char *text, bool gold, bool new);

/** @brief The progress of an achievement as a short banner, only the latest counts. */
void banner_show_progress(const char *progress, const char *title, bool gold);

/** @brief Clears the banners once another game runs. */
void banner_follow_game(void);

/** @brief One poll's worth of banner work, fills header bytes 6 and 7. */
void banner_step(unsigned char *hdr);

/** @brief A message when the RA task's state changes, then the mode. */
void banner_login(void);

/** @brief The server's answer to a leaderboard entry. */
void banner_lboard(void);

/** @brief The mode is said again at the next chance. */
void banner_ask_mode(void);

/** @brief A banner on its way or shown starts over, for a new core. */
void banner_restart(void);

#endif /* BANNER_H */
