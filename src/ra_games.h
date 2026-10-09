/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_games.h
 *  @brief The games the firmware knows: their ROM files, ids, hashes and boards, see ra_games.c.
 *
 *  The table is a file of its own that CMake takes from GAME20K_GAMES_TABLE. game20k's
 *  scripts/build_companion.sh passes the one it generates from its ROM manifests, a build
 *  without it links ra_games_example.c, which holds no game. */
#ifndef RA_GAMES_H
#define RA_GAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <ff.h>

#define RA_GAMES_HASH_LEN 32       /**< md5 in hex, what the server knows a game by */
#define RA_GAMES_V1_ID    12138u   /**< card files and queue lines without a game are Galaga's: the firmware before this one knew no other game */
#define RA_GAMES_V1_HASH  "b8140b5e33c53b0f7dd3cc368951a4dd"   /**< md5("galaga"), the hash of RA_GAMES_V1_ID */
#define RA_GAMES_DIR      "/sd/ra"   /**< one folder per game below it, named by the game's id */
#define RA_GAMES_PATH_MAX 40         /**< "/sd/ra/4294967295/patch.json.new" is 32 characters, 33 with the NUL */

#define RA_GAMES_SET_FILE   "patch.json"       /**< in the game's folder: the server's reply to r=patch */
#define RA_GAMES_SET_TMP    "patch.json.new"   /**< a new set while it is written, then it takes the place of the old */
#define RA_GAMES_SET_MAC    "patch.mac"        /**< "g20k-s1 <tag>": the set file's tag with the device key */
#define RA_GAMES_STATE_FILE "unlocked.txt"     /**< the account's unlocks of this game, see ra_state.c */

/** @brief A DIP switch the set expects at one value, by the switch's id in the core's XML. */
typedef struct {
  char id;      /**< the id of the list in the core's XML, e.g. 'L' */
  int  value;   /**< the listentry value the set expects, as the menu keeps it and the core gets it */
} ra_dip_t;

/** @brief One ROM file hardcore accepts. */
typedef struct {
  unsigned char sha[32];   /**< SHA-256 of the file as it streams to the core */
  const char   *label;     /**< the manifest's known line, e.g. "with the 54xx", at most 16 characters */
} ra_rom_t;

/** @brief One game of the table. */
typedef struct {
  const char     *set;     /**< MAME set name, the card file is the set name plus ".rom" */
  const char     *title;   /**< for the menu */
  unsigned        id;      /**< the game's id on the server */
  const char     *hash;    /**< md5 of set, 32 hex, precomputed by the table's generator */
  unsigned char   board;   /**< header byte 12 of the core that runs it, the manifest's board line */
  const ra_rom_t *roms;    /**< the files hardcore accepts */
  unsigned        rom_n;   /**< how many */
  const ra_dip_t *dips;    /**< switches the set expects, NULL for none */
  unsigned        dip_n;   /**< how many */
} ra_game_t;

/** @brief The table, one row per game. Several sets may share a board, the digest then picks
 *         the set, and the first row of a board is the game shown without a ROM. */
extern const ra_game_t ra_games_rows[];
extern const unsigned  ra_games_rows_n;   /**< rows in ra_games_rows */
extern const char      ra_games_origin[]; /**< "generated" for game20k's table, "example" for ra_games_example.c */

/** @brief What ra_games_select() decided for this boot. */
typedef struct {
  const ra_game_t *game;       /**< the entry, NULL for a fallback identity or no game */
  unsigned         id;         /**< the id the server is asked with, 0 for a fallback identity (the server resolves it), a wrong board or no game */
  char             hash[RA_GAMES_HASH_LEN + 1];   /**< what the server is asked with, "" when there is no game to play */
  const char      *rom_label;  /**< the known file's label, "ROM unknown", "ROM not checked" (the stream still runs), "no ROM" */
  bool             rom_ok;     /**< the digest is a known file of game: no RA_HC_BLOCK_ROM */
  bool             board_ok;   /**< board known and game belongs to it: no RA_HC_BLOCK_GAME */
} ra_ident_t;

/** @brief The first entry of that board, NULL when there is none. */
const ra_game_t *ra_games_by_board(unsigned char board);
/** @brief The entry with that hash, 32 hex compared without case, NULL when there is none. */
const ra_game_t *ra_games_by_hash(const char *hex);
/** @brief The entry with that id, NULL when there is none. Several entries share an id
 *         (a game and its regional sets), this is the first of them. */
const ra_game_t *ra_games_by_id(unsigned id);
/** @brief The row of an entry of the table, which tells the entries apart where the id
 *         does not. g must be an entry of the table. */
unsigned ra_games_row(const ra_game_t *g);
/** @brief The entry in that row, NULL beyond the table. */
const ra_game_t *ra_games_at(unsigned row);
/** @brief The entry whose card file is name, the set name plus ".rom", case ignored, NULL when there is none. */
const ra_game_t *ra_games_by_file(const char *name);
/** @brief The entry one of whose files has that digest, any board, NULL when there is none.
 *
 *  row, when not NULL, gets the file's row. */
const ra_game_t *ra_games_by_rom(const unsigned char sha[32], const ra_rom_t **row);

/** @brief RetroAchievements' arcade rule: the md5 of the file name without path and without its last extension.
 *
 *  Bytes as they are, as rc_hash_arcade() does it (rcheevos hash_rom.c). hex gets 32
 *  lowercase digits and a NUL. */
void ra_games_name_hash(const char *image, char hex[RA_GAMES_HASH_LEN + 1]);

/** @brief Decides the game of this boot from the board id, the ROM digest and the image name's hash.
 *
 *  board 0 means no layout 4 header was read. sha NULL means no digest, name_hash
 *  NULL that nothing was streamed, streaming that the stream has not ended yet
 *  (the label then says "ROM not checked" instead of "ROM unknown"). The rules,
 *  in order: a known digest names its game; else a name hash of a table game
 *  names that game (softcore, the file is not proven); else the name hash is a
 *  fallback identity the server resolves, on a board the table knows to one of
 *  that board's games only (ra_patch_resolved), on a board without a table entry
 *  to whatever the server says; without a name the board's first game is shown
 *  with "no ROM". A game that does not belong to the board is not played: the
 *  entry stays for the menu, the hash is "" and the id 0. */
void ra_games_select(unsigned char board, const unsigned char *sha, const char *name_hash, bool streaming, ra_ident_t *out);

/** @brief The path of file in the game's folder, "/sd/ra/12138/patch.json" for id 12138, into buf. False when id is 0 or it does not fit. */
bool ra_games_path(unsigned id, const char *file, char *buf, size_t n);

/** @brief Makes RA_GAMES_DIR and the game's folder below it. Caller holds sdc_lock.
 *
 *  FR_OK when both exist afterwards, which includes both having existed. FR_NO_PATH
 *  when RA_GAMES_DIR or the game's folder is a plain file (one log line names it),
 *  or id is 0. */
FRESULT ra_games_mkdir(unsigned id);

#endif /* RA_GAMES_H */
