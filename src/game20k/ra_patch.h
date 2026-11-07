/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.h
 *  @brief The current game and its achievement set, read from the card, see ra_patch.c. */
#ifndef RA_PATCH_H
#define RA_PATCH_H

#include <stdbool.h>
#include "rc_runtime.h"
#include "game.h"

/** @brief True while the ROM in the core is another game's than the running one, from a later settle. com_task.
 *
 *  ra_patch_apply_pending() takes the set out of rcheevos meanwhile, ra_queue_add()
 *  drops what would still come. */
bool ra_patch_foreign_rom(void);

/** @brief The id the server resolved the hash of the game of generation gen to. RA task, after r=gameid.
 *
 *  Nothing happens once that game no longer runs. For a table entry a differing
 *  id raises RA_HC_BLOCK_GAME, the table's id is kept. For a fallback identity the
 *  id is stored and its card folder made: on a board the table knows only when it
 *  is one of that board's table games, on a board without a table entry whatever
 *  the server says. With the board still unknown (no RAM mirror header read)
 *  nothing is played. Anything else, and 0 (the server does not know the hash),
 *  leaves no game. */
void ra_patch_resolved(unsigned gen, unsigned server_id);

/** @brief Sets up the handover of a parsed set to com_task. Once, before the RA task runs. False when it failed. */
bool ra_patch_init(void);

#define RA_PATCH_RESET   1u /**< ra_patch_apply_pending(): the core was reset, a game starts */
#define RA_PATCH_NEW_SET 2u /**< ra_patch_apply_pending(): a new set is active, table positions changed */

/** @brief Activates a set the RA task handed over, if one waits. com_task only, before each frame.
 *
 *  A set handed over for a game that no longer runs is dropped. Achievements of
 *  the previous set that the new one does not carry are deactivated.
 *  After ra_patch_core_reset() it first resets rcheevos. Returns RA_PATCH_RESET and
 *  RA_PATCH_NEW_SET for what happened, 0 when nothing did. */
unsigned ra_patch_apply_pending(rc_runtime_t *rt);

/** @brief The core has been reset. Any task, it only sets a flag.
 *
 *  Before the next frame, rcheevos starts over: hit counts, leaderboards and rich
 *  presence begin anew, so nothing of the previous game carries into the next. */
void ra_patch_core_reset(void);

/** @brief Reads the card set once more and hands it over, when com_task asked for it. RA task only, never while a set fetch is in flight.
 *
 *  com_task asks (and wakes the task, ra_task_wake()) when the game's ROM is back
 *  after another game's file, see ra_patch_foreign_rom(), and for a new game, see
 *  game_new(). The set is the running game's at the time of the read.
 *  Nothing happens otherwise, so it is cheap to call at every event. True when nothing was asked
 *  for or the card gave a set with a valid tag; false when it did not (no file, no
 *  tag, unusable), the caller then fetches the set from the server again. */
bool ra_patch_set_again(void);

/** @brief True from the ask of ra_patch_set_again() until com_task took the set it handed over, or the card had none. com_task only.
 *
 *  The mode banner waits for it: before, the set's hardcore block still holds and
 *  the banner would say softcore for a game whose set is on the card. A set handed
 *  over while a foreign ROM runs does not count, com_task takes it only once the
 *  game's ROM is back. */
bool ra_patch_set_pending(void);

/** @brief The buffer for a set from the server, and its size. RA task only. */
char *ra_patch_body(unsigned *cap);

/** @brief Takes the server's reply of len bytes in that buffer, asked for the game of generation gen. RA task only.
 *
 *  Undoes the chunked framing, parses the set, keeps it on the card when it
 *  differs from the set the card had, and hands it to com_task. Returns 1 for a
 *  new set, 0 when the card already had this one or the game no longer runs (the
 *  reply is dropped), -1 when the reply is no set. A game that moves on while this
 *  runs gets nothing of it: the card file goes to the folder of the game it was
 *  asked for, the hardcore block and the handover check the generation. */
int ra_patch_from_server(unsigned len, unsigned gen);

#define RA_PATCH_TITLE_MAX    32  /**< titles are cut to 31 characters, the FPGA banner shows 24 */
#define RA_PATCH_DESC_MAX     256 /**< descriptions up to 255 characters, as the server allows them */
#define RA_PATCH_PROGRESS_MAX 24  /**< measured progress, "4294967295/4294967295" fits */

/** @brief One achievement of the active set, as the menu shows it. */
typedef struct {
  unsigned id;                          /**< achievement id on the server */
  unsigned points;                      /**< its points */
  char title[RA_PATCH_TITLE_MAX];       /**< title, cut to fit */
  char desc[RA_PATCH_DESC_MAX];         /**< description, cut to fit */
  char progress[RA_PATCH_PROGRESS_MAX]; /**< measured progress, "" when it has none */
  bool primed;                          /**< challenge on: all its conditions but the trigger hold */
} ra_patch_item_t;

/** @brief Copies achievement i (0-based, in the set's order) to out. Any task.
 *
 *  False when there is no such achievement. */
bool ra_patch_item(unsigned i, ra_patch_item_t *out);

/** @brief The progress of an achievement as text, "12/50" or "37%", "" when there is none. com_task only.
 *
 *  Empty also while the value is unknown or 0, e.g. right after a reset, and below
 *  1 percent, as rc_client shows it. */
void ra_patch_format_progress(const rc_runtime_t *rt, unsigned id, char *buf, size_t size);

/** @brief Reads progress and challenge state of every achievement from rcheevos. com_task only, after a frame.
 *
 *  About once a second is enough, the menu shows what it read last. */
void ra_patch_update_progress(const rc_runtime_t *rt);

#define RA_PATCH_MAX    128 /**< achievements kept per set, e.g. Galaga 17, Pang 114 */
#define RA_PATCH_LB_MAX 12  /**< leaderboards kept per set, e.g. Galaga 1, Ms. Pac-Man 10 */

/** @brief One leaderboard of the active set. */
typedef struct {
  unsigned id;                    /**< leaderboard id on the server */
  int format;                     /**< how its value reads, for rc_runtime_format_lboard_value() */
  bool lower_is_better;           /**< e.g. a time */
  char title[RA_PATCH_TITLE_MAX]; /**< title, cut to fit */
} ra_patch_lboard_t;

/** @brief Copies the leaderboard with this id to out. Any task. False when the set has none such. */
bool ra_patch_lboard(unsigned id, ra_patch_lboard_t *out);

/** @brief The server's warning about this client, e.g. "Unknown Emulator". Any task.
 *
 *  RetroAchievements adds it to the set as an achievement while it has not
 *  fully approved the client for hardcore, e.g. an unknown or outdated one. The
 *  server then records hardcore unlocks as casual and keeps no leaderboard
 *  entries, whatever mode the device is in. It may warn an approved client with
 *  a known issue as well, which this firmware does not tell apart: such a
 *  client's results would read as not recorded. Copies its title without
 *  "Warning: " to out, which may be NULL, and returns true when the set parsed
 *  last, from the card or the server, has one. */
bool ra_patch_warning(char *out, size_t size);

/** @brief Number of core achievements in the active set. */
unsigned ra_patch_count(void);
/** @brief 1-based position of an achievement id in the set, 0 when not in it. */
unsigned ra_patch_index(unsigned id);
/** @brief Title of an achievement, "" when unknown. */
const char *ra_patch_title(unsigned id);

#endif /* RA_PATCH_H */
