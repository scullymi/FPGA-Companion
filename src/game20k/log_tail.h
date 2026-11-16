/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file log_tail.h
 *  @brief The last lines of the log in RAM a restart keeps, see log_tail.c. BL616 only. */
#ifndef LOG_TAIL_H
#define LOG_TAIL_H

#include <stdbool.h>

/** @brief debugf() of the BL616 build: prints the line and keeps it in the tail. Any task. */
void log_tail_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** @brief Marks the next part as one whose log the console may not carry, e.g. a JTAG load with the FPGA erased. */
void log_tail_arm(bool on);

/** @brief Prints what the armed part logged so far, e.g. from a timer when it hangs. */
void log_tail_print_armed(void);

/** @brief After a restart from an armed part: prints the tail it left, once. Call when the console works again. */
void log_tail_dump(void);

#endif /* LOG_TAIL_H */
