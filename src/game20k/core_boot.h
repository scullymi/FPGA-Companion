/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file core_boot.h
 *  @brief Setting up a core, see core_boot.c and core_config() in main.c. */
#ifndef CORE_BOOT_H
#define CORE_BOOT_H

#include <stdbool.h>

#include "../mcu_hw.h"

/** @brief main.c: the menu of the core that runs, parsed into cfg, true for a game20k core whose menu this firmware lacks. */
bool core_config(unsigned char probe);

/** @brief main.c: the images of the menu's init action, the game picked before, the settle and the ready action. */
void core_start(void);

/** @brief A config.xml from the card is the menu: no hardcore with it. */
void core_config_file(void);

/** @brief The menu without a config.xml, *mismatch true for a game20k core whose menu this firmware lacks. */
char *core_menu(unsigned char probe, bool *mismatch);

/** @brief After the images are open: the Games page without a ROM, the settle without a stream. */
void core_images_open(void);

/** @brief Sets up a new core as the start sets one up, probe is the verdict on its header, t0 when the switch began. */
void core_setup(unsigned char probe, TickType_t t0);

#endif /* CORE_BOOT_H */
