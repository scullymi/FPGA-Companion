/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_task.h
 *  @brief The RetroAchievements task, see ra_task.c. */
#ifndef RA_TASK_H
#define RA_TASK_H

#define RA_CLOCK_VALID 1735689600u   /**< 2025-01-01, an earlier time() means NTP has not set the clock yet */

/** @brief Starts the task. Once, from com_task after the card and config.ini are ready. */
void ra_task_start(void);

/** @brief Tells the task that the clock is set. From sntp_set_system_time(). */
void ra_task_clock_set(void);

#endif /* RA_TASK_H */
