/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_games.c
 *  @brief The lookups on the game table and the choice of the game a boot plays.
 *
 *  A game is a MAME set name, its title, its id and hash on RetroAchievements,
 *  the board (header byte 12 of the core that runs it), the ROM files hardcore
 *  accepts, as SHA-256 of the file that streams to the core, and the DIP switches
 *  its set expects. The rows live in a file of their own (ra_games_rows, see
 *  ra_games.h), so a new game needs no change here.
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

const ra_game_t *ra_games_by_board(unsigned char board) {
  unsigned i;
  if(!board) return NULL;   // 0 is "no header read", never a board
  for(i = 0; i < ra_games_rows_n; i++) if(ra_games_rows[i].board == board) return &ra_games_rows[i];
  return NULL;
}

const ra_game_t *ra_games_by_hash(const char *hex) {
  unsigned i;
  if(!hex) return NULL;
  for(i = 0; i < ra_games_rows_n; i++) if(!strcasecmp(ra_games_rows[i].hash, hex)) return &ra_games_rows[i];
  return NULL;
}

unsigned ra_games_row(const ra_game_t *g) {
  return (unsigned)(g - ra_games_rows);
}

const ra_game_t *ra_games_at(unsigned row) {
  return row < ra_games_rows_n ? &ra_games_rows[row] : NULL;
}

const ra_game_t *ra_games_by_id(unsigned id) {
  unsigned i;
  if(!id) return NULL;
  for(i = 0; i < ra_games_rows_n; i++) if(ra_games_rows[i].id == id) return &ra_games_rows[i];
  return NULL;
}

const ra_game_t *ra_games_by_file(const char *name) {
  unsigned i;
  if(!name) return NULL;
  for(i = 0; i < ra_games_rows_n; i++) {
    // the set name first, then nothing but the extension. strncasecmp stops at the
    // end of a shorter name, so name + n is inside it
    size_t n = strlen(ra_games_rows[i].set);
    if(!strncasecmp(name, ra_games_rows[i].set, n) && !strcasecmp(name + n, ".rom")) return &ra_games_rows[i];
  }
  return NULL;
}

const ra_game_t *ra_games_by_rom(const unsigned char sha[32], const ra_rom_t **row) {
  unsigned i, j;
  for(i = 0; i < ra_games_rows_n; i++)
    for(j = 0; j < ra_games_rows[i].rom_n; j++)
      if(!memcmp(ra_games_rows[i].roms[j].sha, sha, 32)) {
        if(row) *row = &ra_games_rows[i].roms[j];
        return &ra_games_rows[i];
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
