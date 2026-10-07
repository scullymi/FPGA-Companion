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
  { { 0xe6,0x58,0x89,0x3d,0x7e,0x85,0x18,0x5e,0x69,0xae,0x1f,0x8c,0xf9,0x1a,0x5a,0x3d,
      0xc6,0xc2,0xc9,0x70,0x66,0x08,0xd3,0x68,0x25,0xa5,0xf2,0x17,0x34,0xfa,0x61,0x9c }, "with the 54xx" },
  { { 0x9a,0xe0,0x4f,0x20,0xd3,0x6b,0x97,0x68,0x0d,0x52,0xb3,0xd1,0x30,0xe0,0x0d,0xd6,
      0xa0,0xcf,0xe6,0x62,0xee,0xae,0x43,0x60,0x0d,0x9a,0x8e,0x38,0x3b,0x4f,0xe3,0xd2 }, "without the 54xx" },
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
// played with the Japanese original. It asks for 3 lives or more, and "Perfect Pac"
// itself for 5, so 5 lives (list 'L' value 3) is the setting that serves every
// achievement of it, with 1 coin 1 credit (list 'C' value 1) as Pac-Man.
static const ra_rom_t roms_puckman[] = {
  { { 0xad,0x4a,0x2c,0x56,0x2a,0xa8,0xb8,0x7d,0x8f,0x1d,0xff,0xcc,0xf4,0x5b,0xf9,0xd2,
      0x13,0xc0,0x45,0xe1,0xb2,0x37,0xe5,0xec,0x11,0x21,0xfd,0xc8,0x80,0xef,0x73,0x87 }, "MAME puckman" },
};
static const ra_dip_t dips_puckman[] = { { 'L', 3 }, { 'C', 1 } };

// Ms. Pac-Man (Midway, 1981), as scripts/make_rom.py builds mspacman.rom from the MAME set
// "mspacman": pacman.rom's layout with the second program bank (u5, u6, u7) at the end, on
// the Pac-Man board. The achievements read only the difficulty and come for normal and for
// hard. Seven of the ten leaderboards count with at most 3 lives only, so the set expects
// 3 lives (list 'L' value 2), the game's default. Coinage and bonus it reads nowhere.
static const ra_rom_t roms_mspacman[] = {
  { { 0xd6,0xd7,0xcf,0xdb,0xd8,0x91,0x53,0xb5,0x72,0xce,0xb6,0xdd,0xc2,0xa1,0x32,0x2f,
      0x1f,0x48,0x1e,0xb6,0x08,0xe8,0x4c,0x5f,0xde,0xe7,0x31,0x9c,0x4c,0x05,0xba,0xe0 }, "MAME mspacman" },
};
static const ra_dip_t dips_mspacman[] = { { 'L', 2 } };

// Pac-Man and Ms. Pac-Man with the speedup hack (MAME clones pacmanf and mspacmnf), on the
// Pac-Man board in the layouts of pacman.rom and mspacman.rom. RetroAchievements lists them as
// games of their own, with the switches of the parent games.
static const ra_rom_t roms_pacmanf[] = {
  { { 0xa1,0x5e,0x0a,0xca,0x97,0x3d,0x1f,0xa4,0x70,0x73,0x59,0x48,0x79,0x9f,0xca,0x15,
      0xab,0x97,0xb3,0xb5,0xbd,0x37,0x85,0xc4,0xd9,0x26,0x56,0x60,0x92,0xbf,0xce,0x3e }, "MAME pacmanf" },
};
static const ra_rom_t roms_mspacmnf[] = {
  { { 0x00,0xbf,0x50,0x8e,0x3d,0x7f,0x62,0xbf,0x6d,0xf9,0x44,0x0d,0xae,0x05,0xd0,0x34,
      0x3a,0x6f,0xbf,0x26,0x8d,0xf3,0xfd,0xba,0x48,0x49,0xc0,0x66,0x0c,0x03,0x2d,0x24 }, "MAME mspacmnf" },
};

// 1942 (Revision B, Capcom 1984), as scripts/make_rom.py builds 1942.rom from the MAME set
// "1942": jotego's layout for jt1942, the sprites sorted as JTFRAME stores them. Only the
// parent set's name is linked on RetroAchievements, its revision A clone is not. The set
// checks no DIP switch: its conditions read the game state, the lives and the score.
static const ra_rom_t roms_1942[] = {
  { { 0x2f,0x84,0x7d,0x48,0xe7,0xe2,0x1c,0xe3,0xf3,0xe0,0x9d,0x5e,0x96,0x11,0xcc,0x47,
      0x42,0xed,0xa6,0xa1,0x34,0xf4,0x68,0xb0,0x7f,0x48,0xda,0xc6,0x9c,0x29,0xe0,0x43 }, "MAME 1942" },
};

// 1943: The Battle of Midway (Capcom 1987), as scripts/make_rom.py builds 1943.rom from
// the MAME set "1943": jotego's layout for jt1943. Only the parent set's name is linked
// to this set on RetroAchievements, "1943mii" has its own. The set checks no DIP switch:
// its conditions read the game state, the credits, the level, the energy and the score.
static const ra_rom_t roms_1943[] = {
  { { 0xce,0xf8,0x9a,0x37,0x1c,0xbc,0xbc,0x02,0x21,0xd7,0xff,0x09,0xc0,0x0b,0xe4,0x85,
      0x43,0xe5,0xd7,0xf5,0xb0,0x6c,0x9b,0x8f,0x0c,0xbc,0x56,0x7e,0xf8,0x66,0xbf,0x44 }, "MAME 1943" },
};

// Ghosts'n Goblins (Capcom 1985), as scripts/make_rom.py builds gng.rom from the gg1 to gg17
// files of roms/gng.zip (the set MAME now calls gngb): jotego's layout for jtgng. The set
// expects 3 lives (list 'L' value 3) and normal difficulty (list 'F' value 3), its rich
// presence reports any other setting as wrong.
static const ra_rom_t roms_gng[] = {
  { { 0x3d,0xde,0x4d,0x4f,0xd2,0x30,0xf8,0x32,0x5d,0xce,0x7a,0x49,0x86,0x62,0x75,0xec,
      0x21,0xbc,0x58,0x9c,0xd9,0xd2,0x92,0xc2,0x2e,0xe5,0xc9,0x7c,0x52,0x13,0x26,0x76 }, "MAME gng (gg)" },
};
static const ra_dip_t dips_gng[] = { { 'L', 3 }, { 'F', 3 } };

// Makaimura, the Japanese version of Ghosts'n Goblins (MAME clone makaimurg), in gng.rom's
// layout on the same board and with the same achievement set and switches.
static const ra_rom_t roms_makaimurg[] = {
  { { 0x36,0x60,0x81,0x6e,0x2b,0x43,0x1f,0x20,0x36,0x4c,0x40,0x03,0x36,0xee,0x23,0xe8,
      0x18,0xf1,0x23,0x39,0xaa,0x9a,0xb2,0xa4,0x32,0xc1,0x37,0x57,0x86,0x17,0x5d,0x3f }, "MAME makaimurg" },
};

// Dig Dug (rev 2, Namco 1982), as scripts/make_rom.py builds digdug.rom from the MAME set
// "digdug" with the programs of the 51xx and the 53xx (namco51.zip, namco53.zip). The set
// expects 3 lives (list 'L' value 2), bonus at 10K, 40K and every 40K (list 'B' value 4)
// and no continue (list 'O' value 1), one of its achievements checks exactly these.
static const ra_rom_t roms_digdug[] = {
  { { 0x32,0x2b,0xcd,0x78,0x52,0x63,0x11,0xd9,0x00,0x56,0x67,0xd4,0x94,0x06,0x4a,0xf5,
      0x19,0x22,0xce,0xeb,0x80,0xfa,0xbf,0x07,0x6e,0x9c,0xa7,0x88,0xbe,0xe4,0xa6,0x78 }, "MAME digdug" },
};
static const ra_dip_t dips_digdug[] = { { 'L', 2 }, { 'B', 4 }, { 'O', 1 } };

// Pang (Mitchell 1989) and Super Pang (1990) on one board, as scripts/make_rom.py builds
// pang.rom and spang.rom from the MAME sets: the program decrypted ahead, jotego's layout
// for jtpang and the EEPROM the game starts with. The boards have no DIP switches. The
// settings live in the EEPROM, the file loads MAME's first-start content every time, and
// both sets read the settings from the game's RAM themselves.
static const ra_rom_t roms_pang[] = {
  { { 0x3a,0x2e,0x95,0x0c,0xf6,0xe5,0x87,0x9a,0x00,0x7d,0x66,0x9e,0xcb,0xfc,0x05,0x64,
      0x57,0x9b,0x14,0x66,0xdf,0x62,0x3f,0x4d,0xe3,0x4a,0x56,0x93,0x9e,0x01,0xc7,0x29 }, "MAME pang" },
};
static const ra_rom_t roms_spang[] = {
  { { 0xef,0xc3,0xce,0xc0,0xf8,0x1b,0xa2,0xda,0xf4,0x98,0x6f,0xad,0x03,0x13,0xa3,0xae,
      0x6f,0x19,0xaa,0xc4,0x73,0xfa,0x5d,0xcb,0x89,0x73,0x7b,0x68,0xb9,0x2b,0x04,0x0f }, "MAME spang" },
};

// Buster Bros. and Super Buster Bros., the US versions of Pang and Super Pang (MAME clones
// bbros and sbbros), on the same board and with the same achievement sets.
static const ra_rom_t roms_bbros[] = {
  { { 0xc1,0xb7,0x09,0xec,0xe3,0x06,0x76,0x5c,0xb4,0x41,0x18,0xdf,0xee,0x4e,0xe2,0x6c,
      0x0a,0x30,0x1b,0x54,0xd4,0x53,0x18,0xe0,0x77,0xdc,0x99,0xba,0x93,0xb1,0xae,0xf0 }, "MAME bbros" },
};
static const ra_rom_t roms_sbbros[] = {
  { { 0xc8,0x82,0x8e,0x85,0xa7,0x4e,0x4c,0xb9,0x83,0x17,0xa1,0x3f,0xda,0x6e,0x4b,0xc5,
      0x2d,0x4e,0x85,0x7d,0x0b,0xd8,0x85,0xa4,0xa6,0xd7,0xed,0x9d,0x8c,0x72,0x9f,0xb2 }, "MAME sbbros" },
};

// Jr. Pac-Man (Bally Midway 1983) on the Pac-Man board, as scripts/make_rom.py builds
// jrpacman.rom from the MAME set "jrpacman": mspacman.rom's layout, then the program
// decrypted with MAME's table and the rest of the graphics and PROMs. The set expects
// 3 lives (list 'L' value 2) and bonus at 20000 (list 'B' value 2), one of its
// achievements checks these switches.
static const ra_rom_t roms_jrpacman[] = {
  { { 0x29,0x9e,0x5f,0xfb,0xf2,0x8c,0x7e,0xcf,0x70,0xa7,0x20,0x12,0xa1,0xa6,0xf6,0x61,
      0x2c,0xb2,0xfb,0xf4,0x5b,0xae,0x93,0xae,0x49,0x35,0x3d,0x61,0x5d,0xf6,0x6b,0x0e }, "MAME jrpacman" },
};
static const ra_dip_t dips_jrpacman[] = { { 'L', 2 }, { 'B', 2 } };

// Time Pilot (Konami 1982), as scripts/make_rom.py builds timeplt.rom from the MAME set
// "timeplt". The set asks for the default settings: 3 lives (list 'L' value 3), bonus at
// 10K and every 50K (list 'B' value 1) and difficulty 4 (list 'F' value 4), MAME's defaults.
static const ra_rom_t roms_timeplt[] = {
  { { 0xdd,0x10,0x89,0xb8,0x68,0xad,0xe9,0xf6,0x56,0xfd,0xa4,0xa5,0xd4,0x13,0x30,0xf9,
      0x0b,0xdd,0xe8,0x07,0x70,0x03,0x97,0x21,0x65,0xf9,0x6d,0xb9,0x14,0x1d,0xbf,0x74 }, "MAME timeplt" },
};
static const ra_dip_t dips_timeplt[] = { { 'L', 3 }, { 'B', 1 }, { 'F', 4 } };

// 1943: The Battle of Midway Mark II (US), MAME 1943mii, on the 1943 board with the same
// protection MCU, as scripts/make_rom.py builds 1943mii.rom from the MAME set.
static const ra_rom_t roms_1943mii[] = {
  { { 0x45,0x6c,0x53,0x1c,0xeb,0x24,0x22,0x53,0xbb,0x29,0xe9,0x2a,0x24,0x42,0x01,0x90,
      0xb0,0xf1,0x58,0x48,0x5b,0xe1,0xab,0x4f,0x59,0xb1,0x15,0x20,0x3f,0xbc,0x90,0xbd }, "MAME 1943mii" },
};

// one row per game: set, title, id, hash, board, its files, its switches. Boards
// are numbered in the order the cores arrive (Galaga 1); several sets may share a
// board, the digest then picks the set, and ra_games_by_board() names the first row.
static const ra_game_t games[] = {
  { "galaga",  "Galaga",   12138u, "b8140b5e33c53b0f7dd3cc368951a4dd", 1, roms_galaga,  2, dips_galaga, 2 },
  { "pacman",  "Pac-Man",  12192u, "64d1f88b9b276aece4b0edcc25b7a434", 2, roms_pacman,  1, dips_pacman, 2 },
  { "puckman", "Puck Man", 24933u, "7775842918a8f43b7f3caf433e8327b7", 2, roms_puckman, 1, dips_puckman, 2 },
  { "mspacman", "Ms. Pac-Man", 11800u, "01052a074f9e7ce8dc823a5dd2155d14", 2, roms_mspacman, 1, dips_mspacman, 1 },
  { "jrpacman", "Jr. Pac-Man", 12191u, "bfb15e976e21c08502545e7b7a42256c", 2, roms_jrpacman, 1, dips_jrpacman, 2 },
  { "1942",    "1942",     11960u, "519c84155964659375821f7ca576f095", 3, roms_1942,    1, NULL, 0 },
  { "timeplt", "Time Pilot", 11902u, "712417f8d15c9cebd3f2fd22a99aba84", 5, roms_timeplt, 1, dips_timeplt, 3 },
  { "1943",    "1943",     11961u, "c3395dd46c34fa7fd8d729d8cf88b7a8", 6, roms_1943,    1, NULL, 0 },
  { "1943mii", "1943 Mark II", 11962u, "97f13fb525c71f803ca6c076397f098b", 6, roms_1943mii, 1, NULL, 0 },
  { "gng",     "Ghosts'n Goblins", 12149u, "64fb7d782f2be42feb84fef5d0530776", 7, roms_gng, 1, dips_gng, 2 },
  { "digdug",  "Dig Dug",  12091u, "b921a42c761dbfff191c1aebe556d6f7", 8, roms_digdug, 1, dips_digdug, 3 },
  { "pang",    "Pang",     11996u, "1fd9da83be5bae079c8188a2e799020c", 9, roms_pang,    1, NULL, 0 },
  { "spang",   "Super Pang", 12239u, "cb0f93018340d3e639053139fb4c8e30", 9, roms_spang, 1, NULL, 0 },
  { "bbros",   "Buster Bros.", 11996u, "e60d52cc2664f3857c8c149c9d7ff62c", 9, roms_bbros, 1, NULL, 0 },
  { "sbbros",  "Super Buster Bros.", 12239u, "110196a9e2732d9088959ef2e59fba1c", 9, roms_sbbros, 1, NULL, 0 },
  { "makaimurg", "Makaimura", 12149u, "4dc5c37d2056ea8be1f968ebecc66ea0", 7, roms_makaimurg, 1, dips_gng, 2 },
  { "pacmanf", "Pac-Man (speedup)", 24885u, "b045a8ffb012b8fd799e4a611abbf59c", 2, roms_pacmanf, 1, dips_pacman, 2 },
  { "mspacmnf", "Ms. Pac-Man (speedup)", 24949u, "f6ea6e625a0fccecd41c7d0404bef2a8", 2, roms_mspacmnf, 1, dips_mspacman, 1 },
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

const ra_game_t *ra_games_by_file(const char *name) {
  unsigned i;
  if(!name) return NULL;
  for(i = 0; i < GAMES_N; i++) {
    // the set name first, then nothing but the extension. strncasecmp stops at the
    // end of a shorter name, so name + n is inside it
    size_t n = strlen(games[i].set);
    if(!strncasecmp(name, games[i].set, n) && !strcasecmp(name + n, ".rom")) return &games[i];
  }
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
