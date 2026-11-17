/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ram_mirror.h
 *  @brief The RAM mirror on SPI target 5, see ram_mirror.c. */
#ifndef RAM_MIRROR_H
#define RAM_MIRROR_H

#include <stdbool.h>
#include <stdint.h>

#define RAM_MIRROR_SLOTS 8 /**< flash slots in the header, bytes 20 to 27 */

extern unsigned char ram_mirror_board; /**< the running core's board id, read only outside ram_mirror.c */
extern uint16_t ram_mirror_tag;        /**< its interface tag, read only outside ram_mirror.c */
/** The board in each slot of the configuration flash: 0 empty, 255 a core without a tag. Read only
    outside ram_mirror.c, and only while ram_mirror_slots_known. */
extern unsigned char ram_mirror_slots[RAM_MIRROR_SLOTS];
extern bool ram_mirror_slots_known; /**< ram_mirror_slots holds the running core's scan of the flash */

/** @brief Board id and interface tag before the menu and the game, 0 for a valid header, else the last verdict. */
unsigned char ram_mirror_probe(void);

/** @brief Another core came up, its header is read anew as at the start. */
void ram_mirror_forget(void);

/** @brief One poll of the mirror: the banner, the game switch, a new snapshot for rcheevos. */
void ram_mirror_poll(void);

#endif /* RAM_MIRROR_H */
