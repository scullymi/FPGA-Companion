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

#define RA_PATCH_ROM_IMAGE 0   /**< the core's image index of the ROM set, "ROM set" in the core's XML */

/** @brief The ROM image starts to stream to the core. com_task, from sdc.c.
 *
 *  Hardcore is off until ra_patch_rom_end() has checked the whole file. */
void ra_patch_rom_start(void);
/** @brief One block of the ROM image as it goes to the core. com_task, from sdc.c. */
void ra_patch_rom_data(const void *data, unsigned len);
/** @brief The whole ROM image went to the core: compare its SHA-256 with the known files. com_task, from sdc.c. */
void ra_patch_rom_end(void);
/** @brief The ROM image was deselected or could not be sent: no known ROM. com_task, from sdc.c. */
void ra_patch_rom_gone(void);

/** @brief The DIP switches the set expects, n of them. NULL when it expects none. */
const ra_dip_t *ra_game_dips(unsigned *n);

/** @brief Sets up the handover of a parsed set to com_task. Once, before the RA task runs. False when it failed. */
bool ra_patch_init(void);

/** @brief Reads the set from the card, parses it and hands it to com_task. Once, by com_task before the game starts.
 *
 *  Before the RA task runs, so the two never share the set buffer. A set with a
 *  valid tag lifts RA_HC_BLOCK_SET right away, and the game can start in hardcore.
 *
 *  Returns the number of core achievements in the file, which com_task will try
 *  to activate, or -1 when there is no usable set. */
int ra_patch_read_card(void);

#define RA_PATCH_RESET   1u   /**< ra_patch_apply_pending(): the core was reset, a game starts */
#define RA_PATCH_NEW_SET 2u   /**< ra_patch_apply_pending(): a new set is active, table positions changed */

/** @brief Activates a set the RA task handed over, if one waits. com_task only, before each frame.
 *
 *  Achievements of the previous set that the new one does not carry are deactivated.
 *  After ra_patch_core_reset() it first resets rcheevos. Returns RA_PATCH_RESET and
 *  RA_PATCH_NEW_SET for what happened, 0 when nothing did. */
unsigned ra_patch_apply_pending(rc_runtime_t *rt);

/** @brief The core has been reset. Any task, it only sets a flag.
 *
 *  Before the next frame, rcheevos starts over: hit counts, leaderboards and rich
 *  presence begin anew, so nothing of the previous game carries into the next. */
void ra_patch_core_reset(void);

/** @brief The buffer for a set from the server, and its size. RA task only. */
char *ra_patch_body(unsigned *cap);

/** @brief Takes the server's reply of len bytes in that buffer. RA task only.
 *
 *  Undoes the chunked framing, parses the set, keeps it on the card when it
 *  differs from the set the card had, and hands it to com_task. Returns 1 for a
 *  new set, 0 when the card already had this one, -1 when the reply is no set. */
int ra_patch_from_server(unsigned len);

#define RA_PATCH_TITLE_MAX    32   /**< titles are cut to 31 characters, the FPGA banner shows 24 */
#define RA_PATCH_DESC_MAX     256  /**< descriptions up to 255 characters, as the server allows them */
#define RA_PATCH_PROGRESS_MAX 24   /**< measured progress, "4294967295/4294967295" fits */

/** @brief One achievement of the active set, as the menu shows it. */
typedef struct {
  unsigned id;                              /**< achievement id on the server */
  unsigned points;                          /**< its points */
  char     title[RA_PATCH_TITLE_MAX];        /**< title, cut to fit */
  char     desc[RA_PATCH_DESC_MAX];          /**< description, cut to fit */
  char     progress[RA_PATCH_PROGRESS_MAX];  /**< measured progress, "" when it has none */
  bool     primed;                          /**< challenge on: all its conditions but the trigger hold */
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

#define RA_PATCH_LB_MAX 8   /**< leaderboards kept per set, e.g. Galaga 1 */

/** @brief One leaderboard of the active set. */
typedef struct {
  unsigned id;                          /**< leaderboard id on the server */
  int      format;                      /**< how its value reads, for rc_runtime_format_lboard_value() */
  bool     lower_is_better;             /**< e.g. a time */
  char     title[RA_PATCH_TITLE_MAX];    /**< title, cut to fit */
} ra_patch_lboard_t;

/** @brief Copies the leaderboard with this id to out. Any task. False when the set has none such. */
bool ra_patch_lboard(unsigned id, ra_patch_lboard_t *out);

/** @brief Number of core achievements in the active set. */
unsigned    ra_patch_count(void);
/** @brief 1-based position of an achievement id in the set, 0 when not in it. */
unsigned    ra_patch_index(unsigned id);
/** @brief Title of an achievement, "" when unknown. */
const char *ra_patch_title(unsigned id);

#endif /* RA_PATCH_H */
