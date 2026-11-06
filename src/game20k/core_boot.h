/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file core_boot.h
 *  @brief Setting up the core that runs, see core_boot.c. */
#ifndef CORE_BOOT_H
#define CORE_BOOT_H

#include <stdbool.h>

#include "../mcu_hw.h"

/** @brief The menu of the core that runs, parsed into cfg, true for a game20k core whose menu this firmware lacks. */
bool core_config(unsigned char probe);

/** @brief The images of the menu's init action, the game picked before, the settle and the ready action. */
void core_start(void);

/** @brief Sets up a new core as the start sets one up, probe is the verdict on its header, t0 when the switch began. */
void core_setup(unsigned char probe, TickType_t t0);

#endif /* CORE_BOOT_H */
