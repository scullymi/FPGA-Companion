/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_task.h
 *  @brief The RetroAchievements task, see ra_task.c. */
#ifndef RA_TASK_H
#define RA_TASK_H

#include <stdbool.h>
#include <stdint.h>

#define RA_CLOCK_VALID 1735689600u   /**< 2025-01-01, an earlier time() means NTP has not set the clock yet */
#define RA_RP_MAX      256           /**< rich presence text with its NUL, as rc_client keeps it */

/** @brief Where the task stands, for the banner. */
typedef enum {
  RA_TASK_STARTING,     /**< nothing decided yet */
  RA_TASK_NO_ACCOUNT,   /**< no user and token under [RA] in config.ini, achievements stay local */
  RA_TASK_CONNECTING,   /**< waiting for the clock, the server or the login */
  RA_TASK_NO_TIME,      /**< a minute without time from NTP, still waiting for it */
  RA_TASK_RETRYING,     /**< the server or the login failed, the next try waits for its pause */
  RA_TASK_LOGGED_IN,    /**< the account is in */
  RA_TASK_REJECTED,     /**< the server refused the account, no new try until the next start */
  RA_TASK_NO_GAME       /**< nothing to talk to the server about: no ROM on an unknown board, a wrong board, a board still unknown when the server answered, or a hash the server does not know */
} ra_task_state_t;

/** @brief The task's state. Any task, it is a single word. */
ra_task_state_t ra_task_state(void);

/** @brief Starts the task. Once, from com_task after the card and config.ini are ready. */
void ra_task_start(void);

/** @brief Tells the task that the clock is set. From sntp_set_system_time(). */
void ra_task_clock_set(void);

/** @brief Wakes the task so it looks around: e.g. for a card set asked for again, ra_patch_set_again(). Any task.
 *
 *  Through the clock's semaphore, the task checks time() itself, so an extra wake
 *  costs nothing. Nothing happens before ra_task_start(). */
void ra_task_wake(void);

/** @brief True when unlocks count as hardcore. Any task.
 *
 *  The session, the pings and every unlock carry this mode. The core's menu sets
 *  it with the value 'H', 1 hardcore and 0 softcore, see ra_task_core_value(). A
 *  core without that value runs in softcore. */
bool ra_task_hardcore(void);

#define RA_HC_BLOCK_CORE 1u    /**< the core was built with diagnostic parameters (RAM mirror header byte 9) */
#define RA_HC_BLOCK_XML  2u    /**< the menu comes from a config.xml on the card, which could set DIP switches without a reset */
#define RA_HC_BLOCK_ROM  4u    /**< the ROM image is not one of the known files, or not loaded yet */
#define RA_HC_BLOCK_SET  8u    /**< the achievement set is not proven: the card's copy without a valid tag, and none from the server yet */
#define RA_HC_BLOCK_KEY  16u   /**< no device key, unlocks cannot be tagged, see ra_mac.c */
#define RA_HC_BLOCK_SIZE 64u   /**< a part of the set found no memory and stays off: in hardcore every achievement and leaderboard must run */
#define RA_HC_BLOCK_GAME 32u   /**< the ROM in the core is not this game's in one of four ways: the board is unknown, the ROM's game does not belong to this board, the ROM was changed to another game after the start, or the server's id differs from the table's */
#define RA_HC_BLOCK_MENU 128u  /**< the core's interface tag differs from the firmware's for its board: the basic menu runs, the core may read the switches otherwise (menus.h) */

/** @brief The reasons that keep hardcore off although the menu asks for it, RA_HC_BLOCK_*. Any task. */
unsigned ra_task_hardcore_blocked(void);

/** @brief True when the menu asks for hardcore, whether or not a reason keeps it off. Any task. */
bool ra_task_hardcore_wanted(void);

/** @brief Sets or clears a reason that keeps hardcore off. Any task.
 *
 *  A reason that appears switches to softcore at once. When the last one goes,
 *  hardcore comes back as the mode switch does: at once while the core is in reset,
 *  otherwise after a reset of the running game. */
void ra_task_hardcore_block(unsigned reason, bool on);

/** @brief Header byte 9 of the RAM mirror: the core's diagnostic parameters. com_task, each snapshot.
 *
 *  Anything but 0 keeps hardcore off (RA_HC_BLOCK_CORE). */
void ra_task_core_flags(unsigned char flags);

/** @brief A value the Companion sends to the core, from sys_set_val(). Any task.
 *
 *  'R' is the core's reset: when it ends, rcheevos starts over. 'H' is the mode.
 *  Softcore applies at once. Hardcore applies at once while the core is in reset,
 *  e.g. at the start, and otherwise the game is reset first, as RetroAchievements
 *  asks: a game that started in softcore never continues in hardcore. */
void ra_task_core_value(char id, int value);

/** @brief Hands the rich presence text of the running game to the task. com_task only.
 *
 *  The next ping sends it. Longer texts are cut to RA_RP_MAX - 1 characters. */
void ra_task_set_richpresence(const char *text);

/** @brief What the server answered to a leaderboard entry, for the banner. */
typedef struct {
  unsigned id;        /**< leaderboard id */
  int32_t  score;     /**< the value submitted */
  int32_t  best;      /**< the account's best value on this leaderboard */
  unsigned rank;      /**< the rank of that best value, also for an entry the server did not keep, see banner_lboard() in main.c */
  unsigned entries;   /**< entries on the leaderboard */
} ra_lboard_result_t;

/** @brief Hands a leaderboard result to the RA task, which submits it. com_task only, never blocks.
 *
 *  Only in hardcore: RetroAchievements keeps leaderboard entries as hardcore ones,
 *  and only from a client it has approved.
 *  Kept in RAM until the server has it, as rc_client does. */
void ra_task_lboard(unsigned id, int32_t score);

/** @brief The server's answer to the latest leaderboard entry. Any task.
 *
 *  True and out filled when there is one newer than *seen, which is then updated. */
bool ra_task_lboard_result(unsigned *seen, ra_lboard_result_t *out);

/** @brief Counts one frame rcheevos evaluated. com_task only.
 *
 *  Pings go out only while frames arrive, as rc_client does. */
void ra_task_frame(void);

/** @brief Leaderboard results handed over and not yet answered by the server. Any task.
 *
 *  They live in RAM only, so a restart of the Pico waits for 0, as long as it can. */
unsigned ra_task_lboard_pending(void);

#endif /* RA_TASK_H */
