/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_task.h
 *  @brief The RetroAchievements task, see ra_task.c. */
#ifndef RA_TASK_H
#define RA_TASK_H

#define RA_CLOCK_VALID 1735689600u   /**< 2025-01-01, an earlier time() means NTP has not set the clock yet */

/** @brief Where the task stands, for the banner. */
typedef enum {
  RA_TASK_STARTING,     /**< nothing decided yet */
  RA_TASK_NO_ACCOUNT,   /**< no user and token under [RA] in config.ini, achievements stay local */
  RA_TASK_CONNECTING,   /**< waiting for the clock, the server or the login */
  RA_TASK_NO_TIME,      /**< a minute without time from NTP, still waiting for it */
  RA_TASK_LOGGED_IN,    /**< the account is in */
  RA_TASK_REJECTED      /**< the server refused the account, no new try until the next start */
} ra_task_state_t;

/** @brief The task's state. Any task, it is a single word. */
ra_task_state_t ra_task_state(void);

/** @brief Starts the task. Once, from com_task after the card and config.ini are ready. */
void ra_task_start(void);

/** @brief Tells the task that the clock is set. From sntp_set_system_time(). */
void ra_task_clock_set(void);

#endif /* RA_TASK_H */
