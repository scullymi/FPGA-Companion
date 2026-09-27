/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_task.h
 *  @brief The RetroAchievements task, see ra_task.c. */
#ifndef RA_TASK_H
#define RA_TASK_H

#include <stdbool.h>

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
  RA_TASK_REJECTED      /**< the server refused the account, no new try until the next start */
} ra_task_state_t;

/** @brief The task's state. Any task, it is a single word. */
ra_task_state_t ra_task_state(void);

/** @brief Starts the task. Once, from com_task after the card and config.ini are ready. */
void ra_task_start(void);

/** @brief Tells the task that the clock is set. From sntp_set_system_time(). */
void ra_task_clock_set(void);

/** @brief True when unlocks count as hardcore. Any task.
 *
 *  The session, the pings and every unlock carry this mode. The core's menu sets
 *  it with the value 'H', 1 hardcore and 0 softcore, see ra_task_core_value(). A
 *  core without that value runs in softcore. */
bool ra_task_hardcore(void);

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

/** @brief Counts one frame rcheevos evaluated. com_task only.
 *
 *  Pings go out only while frames arrive, as rc_client does. */
void ra_task_frame(void);

#endif /* RA_TASK_H */
