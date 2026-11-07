/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file game.c
 *  @brief What game runs: the board of the core, the ROM that streamed to it, the game the table
 *  says that is, and the game a core switch goes to.
 *
 *  Which game: decided by game_settle() from the board id of the core (RAM mirror header), the
 *  SHA-256 of the ROM image and the image's name, with the rules of ra_games_select(). A known
 *  ROM of another game of this board streamed later becomes the game in place (game_new()).
 *
 *  Whoever keeps something of a game registers with game_events_set() and hears of every change,
 *  in the task and at the moment it happens. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <FreeRTOS.h>
#include <task.h>
#include "mbedtls/sha256.h"
#include "../debug.h"
#include "game.h"

/* The identity of the running game, written by com_task alone: game_settle() at
   the start, game_new() for another game of this board. Every write runs
   with interrupts off, a new game counts the generation in the same piece, so on
   this single core game_now() always copies one game's identity. The game table
   (ra_games.c) names the games, this only says which one runs. */
static const ra_game_t *volatile game; // the table entry, NULL for a fallback identity or no game
static unsigned running_id;            // its id, 0 for a fallback identity, a wrong board or no game
static char running_hash[RA_GAMES_HASH_LEN + 1]; // what the server is asked with, "" when there is no game
static volatile unsigned resolved_id;            // fallback identity: the id the server gave, 0 until then
static const char *volatile rom_label = "";      // what the ROM file is, for the Version dialog
static volatile unsigned generation;             // the games of this start, see game_generation()
static volatile unsigned char board;             // header byte 12, 0 until a valid header was read
static bool settled;                             // game_settle() ran
static bool new_core; // a core switch ran, the next settle makes the new core's game, see game_new_core()
static const ra_game_t *volatile switch_to; // the game a core switch starts, see game_switch_to()
static volatile bool
    switch_committed;       // switch_step() writes the marker now: picks no longer change the target
static unsigned pick_seq;   // picks of a game of another board, counted
static unsigned stream_seq; // pick_seq when the ROM that streams now started

/* What the ROM stream leaves behind for settle. rom_start and rom_gone may run in
   menu_task, rom_data and rom_end in com_task, settle in com_task. The stream orders
   start, end and settle on the normal paths; the 15 s timeout settle of the main
   loop is not ordered by it, so rom_start and rom_gone publish their stores with
   interrupts off and settle takes its inputs the same way, whole or not at all. */
static mbedtls_sha256_context rom_sha; // the ROM image while it streams
static bool rom_hashing;
static volatile bool
    rom_pending; // rom_start ran, rom_end or rom_gone did not yet: the main loop holds its settle back
static volatile TickType_t rom_started; // when the stream that runs now began, see game_rom_streaming()
static unsigned char rom_sum[32];       // its SHA-256 once the stream ended
static bool rom_sum_valid;
static volatile bool rom_bad; // the ROM in the core is damaged, or none came for an old file: no game
static char
    rom_name_hash[RA_GAMES_HASH_LEN + 1]; // md5 of the image's name (arcade rule), "" when nothing streamed

static const game_events_t *events; // who keeps something of a game, NULL for nobody

/* The id of the running game in the table, false when generation gen no longer runs. */
bool game_table_id(unsigned gen, unsigned *id) {
  bool now;
  taskENTER_CRITICAL();
  now = gen == generation;
  *id = running_id;
  taskEXIT_CRITICAL();
  return now;
}

/* The id the server gave a fallback identity, false when generation gen no longer runs. */
bool game_resolve(unsigned gen, unsigned id) {
  bool now;
  taskENTER_CRITICAL();
  now = gen == generation;
  if(now) resolved_id = id;
  taskEXIT_CRITICAL();
  return now;
}

void game_events_set(const game_events_t *e) { events = e; }

void game_now(game_now_t *out) {
  taskENTER_CRITICAL();
  out->gen = generation;
  out->id  = running_id ? running_id : resolved_id;
  memcpy(out->hash, running_hash, sizeof(out->hash));
  taskEXIT_CRITICAL();
}
unsigned game_generation(void) { return generation; }
bool game_current(unsigned gen) { return gen == generation; }

const char *game_hash(void) { return running_hash; }
unsigned game_id(void) {
  unsigned id;
  taskENTER_CRITICAL(); // both of one game
  id = running_id ? running_id : resolved_id;
  taskEXIT_CRITICAL();
  return id;
}
// the entry once: a new game may change it between two reads
const char *game_title(void) {
  const ra_game_t *g = game;
  return g ? g->title : NULL;
}
unsigned char game_board(void) { return board; }
const char *game_rom_label(void) { return rom_label; }
// the switches of the game that is played: a table entry on the wrong board is
// not played, so its switches do not matter either
const ra_dip_t *game_dips(unsigned *n) {
  const ra_game_t *g;
  taskENTER_CRITICAL(); // entry and id of one game
  g = running_id ? game : NULL;
  taskEXIT_CRITICAL();
  *n = g ? g->dip_n : 0;
  return *n ? g->dips : NULL;
}

void game_rom_start(const char *name) {
  char hex[RA_GAMES_HASH_LEN + 1];
  rom_started = xTaskGetTickCount(); // before rom_pending: a reader never sees an old start
  rom_pending = true;
  if(events) events->rom_unsure();
  // the name's hash right away, not the name: a long name (up to 255 characters)
  // would not fit a small buffer, and the hash is all the arcade rule needs. Made
  // in a local and published whole: the 15 s timeout settle in com_task can read
  // rom_name_hash while menu_task is in here
  if(name) ra_games_name_hash(name, hex);
  else hex[0] = 0;
  taskENTER_CRITICAL();
  memcpy(rom_name_hash, hex, sizeof(rom_name_hash));
  rom_sum_valid = false;
  rom_bad       = false;
  stream_seq    = pick_seq;
  taskEXIT_CRITICAL();
  mbedtls_sha256_init(&rom_sha);
  rom_hashing = mbedtls_sha256_starts(&rom_sha, 0) == 0;
}

void game_rom_data(const void *data, unsigned len) {
  if(rom_hashing && mbedtls_sha256_update(&rom_sha, data, len) != 0) rom_hashing = false;
}

bool game_rom_end(const unsigned char want[32]) {
  rom_sum_valid = rom_hashing && mbedtls_sha256_finish(&rom_sha, rom_sum) == 0;
  mbedtls_sha256_free(&rom_sha);
  rom_hashing = false;
  rom_pending = false;
  // a content that is not what the footer names is no game at all: the core stays
  // in reset, and a guess by the file's name would start a session for nothing
  rom_bad = rom_sum_valid && want && memcmp(rom_sum, want, 32) != 0;
  if(rom_sum_valid)
    debugf("RA: ROM image SHA-256 %02x%02x%02x%02x...%s", rom_sum[0], rom_sum[1], rom_sum[2], rom_sum[3],
           rom_bad ? ", not the footer's: damaged, no game" : "");
  else debugf("RA: hashing the ROM image failed, it counts as unknown");
  return rom_bad;
}

void game_rom_rejected(void) {
  // only while no game is decided: later the core keeps the ROM it runs
  if(!settled) rom_bad = true;
}

void game_rom_gone(void) {
  // no FatFs and no settle here: this runs in menu_task (a file ejected in the
  // OSD, sdc_image_open). Only the ROM block bit moves, and without a name the
  // next settle applies its "no ROM" rule. A file that replaces the ROM, or fails
  // to, does not come here: the core keeps the ROM it has until a new stream
  // starts, and rom_start takes over then.
  if(rom_hashing) mbedtls_sha256_free(&rom_sha);
  rom_hashing = false;
  // published whole, as rom_start does: the 15 s timeout settle in com_task may
  // read these while menu_task is in here
  taskENTER_CRITICAL();
  rom_pending      = false;
  rom_sum_valid    = false;
  rom_name_hash[0] = 0;
  taskEXIT_CRITICAL();
  if(events) events->rom_unsure();
}

bool game_rom_pending(void) { return rom_pending; }
bool game_rom_streaming(unsigned limit_ms) {
  return rom_pending && (TickType_t)(xTaskGetTickCount() - rom_started) < pdMS_TO_TICKS(limit_ms);
}
const ra_game_t *game_switch_to(void) { return switch_to; }

void game_switch_cancel(void) {
  taskENTER_CRITICAL();
  switch_to        = NULL;
  switch_committed = false;
  stream_seq       = pick_seq; // the dropped pick holds no later settle back
  taskEXIT_CRITICAL();
}

bool game_switch_commit(const ra_game_t *g) {
  bool fixed;
  taskENTER_CRITICAL(); // a pick in the menu task must not slip in between
  fixed = g && switch_to == g;
  if(fixed) switch_committed = true;
  taskEXIT_CRITICAL();
  return fixed;
}

bool game_pick_other_board(const char *name) {
  const ra_game_t *g = ra_games_by_file(name);
  bool taken         = false;
  taskENTER_CRITICAL(); // the settle may change the target meanwhile, see game_settle()
  if(switch_committed)
    // the switch is under way: a game of another board comes too late and is
    // dropped. Any other file goes on to sdc_image_open(). A switch without a
    // restart parks the menu task first, so no pick comes then. In the restart
    // that is its fallback the file waits for the card lock, and the Pico goes first
    taken = g && board && g->board != board;
  else if(g && board && g->board != board) {
    switch_to = g; // the newest pick wins
    pick_seq++;    // also over a stream that started before it, see the settle
    taken = true;
  }
  taskEXIT_CRITICAL();
  return taken;
}

void game_set_board(unsigned char b) { board = b; }
bool game_settled(void) { return settled; }

static void identity(const ra_ident_t *ident, bool next) {
  taskENTER_CRITICAL();
  if(next) generation++;
  game        = ident->game;
  running_id  = ident->id;
  resolved_id = 0;
  rom_label   = ident->rom_label;
  memcpy(running_hash, ident->hash, sizeof(running_hash));
  taskEXIT_CRITICAL();
}

void game_settle(void) {
  ra_ident_t ident;
  unsigned char sum[32];
  char name_hash[RA_GAMES_HASH_LEN + 1];
  bool sum_valid, streaming, bad;
  // the inputs whole: rom_start in menu_task may be writing them when the 15 s
  // timeout of the main loop brings com_task here
  taskENTER_CRITICAL();
  memcpy(sum, rom_sum, sizeof(sum));
  memcpy(name_hash, rom_name_hash, sizeof(name_hash));
  sum_valid = rom_sum_valid;
  streaming = rom_pending;
  bad       = rom_bad;
  taskEXIT_CRITICAL();
  ra_games_select(board, sum_valid ? sum : NULL, name_hash[0] ? name_hash : NULL, streaming, &ident);
  if(bad) {
    // a damaged ROM, or none for an old file: no game, neither by the name nor by
    // the board, so the RA task asks the server nothing ("RA: NO GAME")
    memset(&ident, 0, sizeof(ident));
    ident.rom_label = "ROM damaged or old";
  }

  if(settled) {
    // a ROM picked in the OSD or on the Games page after the start. A known ROM of
    // another game of this board, proven by its digest, becomes the game in place,
    // e.g. puckman.rom in the Pac-Man core. Any other file leaves identity, session
    // and card files: the two block bits and the label of the Version dialog follow
    // it, and a file of another game switches the set off, ra_patch_apply_pending()
    // takes it out of rcheevos, until the game's ROM is back in the core.
    // After a core switch the new core's ROM makes the game whatever it is, as the
    // first settle does: the running "no game" is the old core's end, not a game
    // whose ROM may come back.
    bool same  = ident.game == game && !strcmp(ident.hash, running_hash);
    bool other = !same && (new_core || (ident.game && ident.rom_ok && ident.board_ok));
    new_core   = false;
    // the newest pick wins: a ROM that streamed to this core takes a pending core
    // switch back, unless a game of another board was picked after this stream
    // began or the switch is under way (switch_step() in game_switch.c)
    const ra_game_t *was;
    bool later;
    taskENTER_CRITICAL(); // the menu may pick a game of another board meanwhile
    was   = switch_to;
    later = stream_seq != pick_seq; // a game of another board was picked after this stream began
    if(!switch_committed && !later) switch_to = NULL;
    taskEXIT_CRITICAL();
    if(later)
      debugf("RA: %s was picked after this ROM, it stays the target",
             was ? was->set : "a game of another board");
    else if(was) debugf("RA: no core switch for %s, the ROM picked last plays here", was->set);
    if(other) {
      game_new(&ident);
      return;
    }
    rom_label = ident.rom_label;
    if(events) events->rom_checked(same, &ident);
    return;
  }
  settled = true;
  identity(&ident, false);
  debugf("RA: board %u, ROM %s: game %s, id %u, hash %s", board, rom_label,
         game            ? game->set
         : ident.hash[0] ? "unknown to the table"
                         : "none",
         running_id, running_hash[0] ? running_hash : "none");
  if(game && !ident.board_ok)
    debugf("RA: %s runs on board %u, not on this one: not played", game->title, game->board);
  if(events) events->first(&ident);
}

void game_new(const ra_ident_t *ident) {
  char was[RA_GAMES_HASH_LEN + 1]; // the old game for the log, its set name or its hash
  snprintf(was, sizeof(was), "%s", game ? game->set : running_hash[0] ? running_hash : "none");
  // the identity whole, and with it what the RA task may still write for the old
  // game: whatever it hands in later checks the generation in a critical section of
  // its own (card fingerprint, warning, resolved id, the set's hardcore block)
  identity(ident, true);
  if(events) events->replaced(ident);
  debugf("RA: board %u, ROM %s: game %s in place of %s, id %u, hash %s", board, rom_label,
         ident->game      ? ident->game->set
         : ident->hash[0] ? "unknown to the table"
                          : "none",
         was, running_id, running_hash[0] ? running_hash : "none");
}

void game_new_core(void) {
  ra_ident_t none;
  // a stream the switch cut never ends: its digest goes, as at an eject
  if(rom_hashing) mbedtls_sha256_free(&rom_sha);
  rom_hashing = false;
  taskENTER_CRITICAL();
  rom_pending      = false;
  rom_sum_valid    = false;
  rom_bad          = false;
  rom_name_hash[0] = 0;
  taskEXIT_CRITICAL();
  memset(&none, 0, sizeof(none));
  none.rom_label = "no ROM";
  game_new(&none);
  new_core = true;
}
bool game_new_core_pending(void) { return new_core; }
