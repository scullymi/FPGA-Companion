/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file games.h
 *  @brief The Games page's list, the game picked there and the start game, see games.c. */
#ifndef GAMES_H
#define GAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "games_file.h"

#define GAMES_START_FILE "/sd/games.ini"   /**< "start=<file>", written by Save settings */

/** @brief One line of the page, a copy for the menu. */
typedef struct {
  char          title[GAMES_TITLE_MAX + 1];   /**< the footer's title, else the file name, cut */
  uint32_t      crc;                          /**< CRC-32 of the file name, what a restart carries */
  unsigned char board;                        /**< board id from the footer */
} games_item_t;

/** @brief The game picked on the page, until the restart takes it. */
typedef struct {
  unsigned      seq;                          /**< counts the picks, tells a newer one apart */
  uint32_t      crc;                          /**< CRC-32 of the file name in the card's root */
  unsigned char board;                        /**< board of its core */
  char          title[GAMES_TITLE_MAX + 1];   /**< for the messages */
} games_pick_t;

/** @brief Reads the list from the card's root: every *.rom with a valid footer and a board of the table. Menu task only.
 *
 *  Sorted by title. Files found damaged since the card last changed stay out. */
void games_read(void);
/** @brief True when the card changed since the last games_read(). Any task. */
bool games_stale(void);
/** @brief The card's root changed, e.g. an FTP upload of name ended: the list is read again and damaged marks go. Any task. */
void games_changed(const char *name);
/** @brief Lines in the list. Menu task only. */
unsigned games_count(void);
/** @brief Copies line i, false when there is none. Menu task only. */
bool games_item(unsigned i, games_item_t *out);

/** @brief The ROM file name in the card's root streamed with a SHA-256 other than its footer's. Any task.
 *
 *  It stays off the list until the card changes. */
void games_damaged(const char *name);

/** @brief The CRC-32 of a file name, as the list and the restart marker carry it. */
uint32_t games_name_crc(const char *name);

/** @brief Picks a game for restart_step() in main.c: the newest pick wins until it is committed. Menu task. */
void games_pick(const games_item_t *it);
/** @brief Copies the pending pick, false when there is none. Any task. */
bool games_picked(games_pick_t *out);
/** @brief Fixes the pick seq for the restart, false when a newer one came or none is pending. */
bool games_pick_commit(unsigned seq);
/** @brief Drops the pick, e.g. when the core switch failed. */
void games_pick_cancel(void);

/** @brief The file in the card's root whose name has that CRC-32, as "/sd/<name>" into path. Caller holds no lock. */
bool games_find(uint32_t crc, char *path, size_t n);

/** @brief Reads the footer of the file at path. Caller holds no lock. False without a valid one. */
bool games_footer_of(const char *path, games_footer_t *out);

/** @brief Save settings: the running ROM file becomes the start game in GAMES_START_FILE. Caller holds no lock.
 *
 *  A ROM outside the card's root, or none, removes the start game. */
void games_save_start(void);

/** @brief The start game at power-on: its name in the root into name, and its footer. Caller holds no lock.
 *
 *  False when none is stored, or its file is gone, has no valid footer or a board
 *  the table does not know. */
bool games_start_file(char *name, size_t n, games_footer_t *ft);

#endif /* GAMES_H */
