/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_games.c
 *  @brief The game table: the only place a game is named, and the pure lookups on it.
 *
 *  A game is a MAME set name, its title, its id and hash on RetroAchievements,
 *  the board (header byte 12 of the core that runs it) and the ROM files hardcore
 *  accepts, as SHA-256 of the file that streams to the core. The digests and the
 *  board come from the game's manifest (fpga/CORE/SET.manifest), the hash is
 *  the md5 of the set name, which is how RetroAchievements identifies an arcade
 *  game. scripts/check_contracts.py compares this file with the manifests and
 *  reads it with regular expressions, so the data keeps this textual shape:
 *
 *    static const ra_rom_t roms_SET[] = {
 *      { { 0x.., ... 32 bytes ... }, "label" },        one row per known line, in the manifest's order
 *    };
 *    static const ra_game_t games[] = {
 *      { "SET", "TITLE", IDu, "32 hex", BOARD, roms_SET, N, dips_SET, M },   one row per game, on one line
 *    };
 *
 *  No task state and no conditions live here: the file links into a host test,
 *  and the only FatFs use is the two path helpers at the end. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <ff.h>
#include "debug.h"
#include "rcheevos/src/rhash/md5.h"
#include "ra_games.h"

// Galaga, as scripts/make_rom.py builds galaga.rom from the MAME set "galaga" (every
// chip checked against MAME's SHA-1): with the 54xx, and with its 1024 bytes as zeros
// when namco54.zip is missing. The set expects 3 lives (list 'L' value 2) and bonus at
// 20K/70K (list 'B' value 2), most of its conditions read these settings from the
// game's RAM and never fire with other ones.
static const ra_rom_t roms_galaga[] = {
  { { 0xaa,0xf7,0xa7,0x25,0x6f,0x8c,0x4e,0x97,0xb3,0x1f,0x05,0x3e,0x68,0x8f,0x24,0xcb,
      0xb3,0x40,0x75,0xa2,0x6a,0xc7,0x1f,0xf0,0x84,0x76,0x51,0xae,0x93,0xb2,0xaf,0x47 }, "with the 54xx" },
  { { 0xec,0x21,0xe5,0x4d,0xaa,0x09,0xf7,0x8b,0x2f,0x58,0xab,0x06,0x0f,0x5c,0xdf,0xbd,
      0x29,0xd5,0x0b,0x29,0x81,0x60,0x41,0xc6,0xca,0xe3,0x82,0x6c,0xb2,0xda,0xbc,0xd5 }, "without the 54xx" },
};
static const ra_dip_t dips_galaga[] = { { 'L', 2 }, { 'B', 2 } };

// Pac-Man (Midway), as scripts/make_rom.py builds pacman.rom from the MAME set "pacman", the
// ten chips in MAME's order. The set expects 3 lives (list 'L' value 2) and 1 coin 1 credit
// (list 'C' value 1): every achievement resets on free play and on more than 3 lives.
static const ra_rom_t roms_pacman[] = {
  { { 0xb1,0xde,0xec,0x6d,0x4a,0xb9,0x67,0xbc,0x01,0x15,0xbc,0xa3,0x7c,0xad,0x19,0xbc,
      0x22,0x77,0xc4,0x82,0x37,0x62,0x0a,0x2e,0x4a,0x41,0x93,0x2d,0x8e,0x5a,0xb2,0x0e }, "MAME pacman" },
};
static const ra_dip_t dips_pacman[] = { { 'L', 2 }, { 'C', 1 } };

// Puck Man (Namco, 1980), as scripts/make_rom.py builds puckman.rom from the MAME set
// "puckman", the sixteen chips in MAME's order, on the Pac-Man board. RetroAchievements
// lists this set name as game 24933, "Pac-Man [Subset - Perfect Pac]": the subset is
// played with the Japanese original. Its switches are Pac-Man's, the subset asks for 3
// lives or more and for 5 in "Perfect Pac" itself.
static const ra_rom_t roms_puckman[] = {
  { { 0xad,0x4a,0x2c,0x56,0x2a,0xa8,0xb8,0x7d,0x8f,0x1d,0xff,0xcc,0xf4,0x5b,0xf9,0xd2,
      0x13,0xc0,0x45,0xe1,0xb2,0x37,0xe5,0xec,0x11,0x21,0xfd,0xc8,0x80,0xef,0x73,0x87 }, "MAME puckman" },
};

// one row per game: set, title, id, hash, board, its files, its switches. Boards
// are numbered in the order the cores arrive (Galaga 1); several sets may share a
// board, the digest then picks the set, and ra_games_by_board() names the first row.
static const ra_game_t games[] = {
  { "galaga",  "Galaga",   12138u, "b8140b5e33c53b0f7dd3cc368951a4dd", 1, roms_galaga,  2, dips_galaga, 2 },
  { "pacman",  "Pac-Man",  12192u, "64d1f88b9b276aece4b0edcc25b7a434", 2, roms_pacman,  1, dips_pacman, 2 },
  { "puckman", "Puck Man", 24933u, "7775842918a8f43b7f3caf433e8327b7", 2, roms_puckman, 1, dips_pacman, 2 },
};
#define GAMES_N (sizeof(games) / sizeof(games[0]))   /**< rows in games[] */

const ra_game_t *ra_games_by_board(unsigned char board) {
  unsigned i;
  if(!board) return NULL;   // 0 is "no header read", never a board
  for(i = 0; i < GAMES_N; i++) if(games[i].board == board) return &games[i];
  return NULL;
}

const ra_game_t *ra_games_by_hash(const char *hex) {
  unsigned i;
  if(!hex) return NULL;
  for(i = 0; i < GAMES_N; i++) if(!strcasecmp(games[i].hash, hex)) return &games[i];
  return NULL;
}

const ra_game_t *ra_games_by_id(unsigned id) {
  unsigned i;
  if(!id) return NULL;
  for(i = 0; i < GAMES_N; i++) if(games[i].id == id) return &games[i];
  return NULL;
}

const ra_game_t *ra_games_by_rom(const unsigned char sha[32], const ra_rom_t **row) {
  unsigned i, j;
  for(i = 0; i < GAMES_N; i++)
    for(j = 0; j < games[i].rom_n; j++)
      if(!memcmp(games[i].roms[j].sha, sha, 32)) {
        if(row) *row = &games[i].roms[j];
        return &games[i];
      }
  return NULL;
}

void ra_games_name_hash(const char *image, char hex[RA_GAMES_HASH_LEN + 1]) {
  md5_state_t md5;
  unsigned char sum[16];
  unsigned i;
  // the base name: after the last '/', up to the last '.', as rc_hash_arcade() takes
  // it. A name without a '.' is hashed whole, rcheevos does the same for a folder.
  const char *base = strrchr(image, '/');
  base = base ? base + 1 : image;
  const char *dot = strrchr(base, '.');
  size_t len = dot ? (size_t)(dot - base) : strlen(base);
  md5_init(&md5);
  md5_append(&md5, (const md5_byte_t *)base, (int)len);
  md5_finish(&md5, sum);
  for(i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", sum[i]);
}

void ra_games_select(unsigned char board, const unsigned char *sha, const char *name_hash, bool streaming, ra_ident_t *out) {
  const ra_rom_t *row = NULL;
  const ra_game_t *g;
  memset(out, 0, sizeof(*out));
  out->rom_label = "no ROM";
  // rule 1: a known digest names its game, wherever it runs. The ROM decides the
  // game, the board decides whether it is played (below).
  if(sha && (g = ra_games_by_rom(sha, &row))) {
    out->game      = g;
    out->rom_label = row->label;
    out->rom_ok    = true;
    out->board_ok  = g->board == board;
  } else if(name_hash) {
    // the content is not checked: the stream had not ended (the 15 s timeout
    // settled, rom_end's settle corrects the label), or the digest is no known file
    out->rom_label = streaming ? "ROM not checked" : "ROM unknown";
    // rule 2: a file named like a table game plays that game's set without proof
    // of the content, softcore. Any other name is a fallback identity: the server
    // resolves the hash, a new core with a new ROM plays through the server's set;
    // on a board the table knows, ra_patch_resolved() takes only one of the
    // board's own games.
    if((g = ra_games_by_hash(name_hash))) {
      out->game     = g;
      out->board_ok = g->board == board;
    } else {
      snprintf(out->hash, sizeof(out->hash), "%s", name_hash);
      out->board_ok = ra_games_by_board(board) != NULL;
    }
  } else if((g = ra_games_by_board(board))) {
    // rule 3: nothing streamed, the board's game is shown and its card set loads
    out->game     = g;
    out->board_ok = true;
  }
  // a table game is played only on its board: a set evaluated against the RAM of
  // another core fires on unrelated bytes. The entry stays for the menu, hash and
  // id say "no game".
  if(out->game && out->board_ok) {
    out->id = out->game->id;
    snprintf(out->hash, sizeof(out->hash), "%s", out->game->hash);
  }
}

bool ra_games_path(unsigned id, const char *file, char *buf, size_t n) {
  int len;
  if(!id) return false;
  len = snprintf(buf, n, "%s/%u/%s", RA_GAMES_DIR, id, file);
  return len > 0 && (size_t)len < n;
}

/* f_mkdir for one level: a name that exists must be a folder. A plain file of that
   name, put there over FTP in softcore or from a computer, would make every open
   below it fail with FR_NO_PATH and the generic "not written" lines only, so it is
   named once per path: the callers come back on every write. */
static FRESULT mkdir_level(const char *path) {
  static char told[RA_GAMES_PATH_MAX];
  FILINFO fi;
  FRESULT r = f_mkdir(path);
  if(r != FR_EXIST) return r;
  if(f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR)) return FR_OK;
  if(strcmp(told, path)) {
    debugf("RA: %s is a file, the card folder cannot be made", path);
    snprintf(told, sizeof(told), "%s", path);
  }
  return FR_NO_PATH;
}

FRESULT ra_games_mkdir(unsigned id) {
  char path[RA_GAMES_PATH_MAX];
  FRESULT r;
  if(!id) return FR_NO_PATH;
  r = mkdir_level(RA_GAMES_DIR);
  if(r != FR_OK) return r;
  // the game's folder, "/sd/ra/<id>", one level per f_mkdir call
  snprintf(path, sizeof(path), "%s/%u", RA_GAMES_DIR, id);
  return mkdir_level(path);
}
