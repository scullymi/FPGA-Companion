/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_games_example.c
 *  @brief EXAMPLE, not the game20k table: a game table without a game.
 *
 *  A build without GAME20K_GAMES_TABLE links this file, so the fork builds on its
 *  own. Such a firmware knows no game: every board is unknown to it, hardcore stays
 *  off, and the Version dialog says "example". game20k's scripts/build_companion.sh
 *  passes the table that scripts/make_fw_tables.py generates from its ROM manifests,
 *  in the same form as below with one row per game, for example:
 *
 *    static const ra_rom_t roms_galaga[] = {
 *      { { 0xe6,0x58,  ... 32 bytes of SHA-256 ... }, "with the 54xx" },
 *    };
 *    static const ra_dip_t dips_galaga[] = { { 'L', 2 }, { 'B', 2 } };
 *    const ra_game_t ra_games_rows[] = {
 *      { "galaga", "Galaga", 12138u, "b8140b5e33c53b0f7dd3cc368951a4dd", 1, roms_galaga, 1, dips_galaga, 2 },
 *    }; */
#include "ra_games.h"

const char ra_games_origin[] = "example";

// C has no empty array: one zero row gives the table a size, ra_games_rows_n keeps
// it out of every lookup
const ra_game_t ra_games_rows[1];
const unsigned  ra_games_rows_n = 0;
