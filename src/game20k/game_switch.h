/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file game_switch.h
 *  @brief The way to another game, see game_switch.c. */
#ifndef GAME_SWITCH_H
#define GAME_SWITCH_H

#include <stdbool.h>

/** @brief One poll's worth of a game switch, true when another core came up without a restart. */
bool switch_step(void);

/** @brief Moves the marker of a restart out of the scratch registers, first of all at the start. */
void marker_take(void);

/** @brief At power-on the start game of Save settings becomes the marker. */
void start_game(void);

/** @brief Before the images are mounted: the ROM of the game the switch was for. */
void switch_rom(void);

/** @brief Hops the flash ring to the board of the marker, true when another core came up.
 *
 *  probe takes the verdict of the header of the last core loaded. */
bool ring_hops(unsigned char *probe);

#endif /* GAME_SWITCH_H */
