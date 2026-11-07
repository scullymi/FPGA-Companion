/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file game.h
 *  @brief What game runs, see game.c. */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include "ra_games.h"

/* The identity of the running game, decided by game_settle() at the start and
   again by game_new() for another game of this board, both in com_task.
   Before settle the accessors say "no game". Each new game counts a generation:
   a task that works for a game over time takes a copy with game_now() and drops
   what it gets back once game_current() says the game moved on. */

/** @brief The identity of the running game as one copy, see game_now(). */
typedef struct {
  unsigned gen;                     /**< the generation it belongs to, see game_generation() */
  unsigned id;                      /**< as game_id() */
  char hash[RA_GAMES_HASH_LEN + 1]; /**< as game_hash() */
} game_now_t;

/** @brief Copies the running game's identity, one game's whole. Any task. */
void game_now(game_now_t *out);
/** @brief Counts the games of this start: 0 for the one settled first, one more per game_new(). Any task. */
unsigned game_generation(void);
/** @brief True while the game of generation gen runs. Any task. */
bool game_current(unsigned gen);

/** @brief Hash the server knows the current game by, an md5 in hex. "" when there is no game.
 *
 *  com_task, which alone changes the identity. Other tasks take game_now(): a
 *  new game rewrites the text in place. */
const char *game_hash(void);
/** @brief Id of the current game on the server. 0 when there is none, or the server has not resolved it yet. Any task. */
unsigned game_id(void);
/** @brief The DIP switches the set expects, n of them. NULL when it expects none, or there is no game. */
const ra_dip_t *game_dips(unsigned *n);
/** @brief Title of the table entry, for the menu. NULL for a fallback identity or before settle. */
const char *game_title(void);
/** @brief Board id of the running core, header byte 12 of the RAM mirror. 0 until a header was read. */
unsigned char game_board(void);
/** @brief What the ROM file is: the known file's label, "ROM unknown", "ROM not checked" (the stream had not ended at settle), "no ROM". "" before settle. */
const char *game_rom_label(void);

#define GAME_ROM_IMAGE 0 /**< the core's image index of the ROM set, "ROM set" in the core's XML */

/* The ROM hooks run in the task that drives the transfer: rom_data and rom_end in
   com_task, rom_start and rom_gone also in menu_task (a file picked, ejected or
   replaced in the OSD) and under sdc_lock. So rom_start and rom_gone do no FatFs
   and no settle: they record what they know and move the ROM block bit, which can
   log a switch to softcore but never resets the core (a block that appears never
   does). The settle that reads what they stored runs in com_task alone. */

/** @brief The ROM image starts to stream to the core, from sdc.c. No FatFs and no settle here, it may hold sdc_lock; only the ROM block bit moves.
 *
 *  name is the file's name for RetroAchievements' arcade rule (NULL: unknown).
 *  Hardcore is off until game_rom_end() has checked the whole file. */
void game_rom_start(const char *name);
/** @brief One block of the ROM image as it goes to the core. com_task, from sdc.c. */
void game_rom_data(const void *data, unsigned len);
/** @brief The whole ROM image went to the core: finish the SHA-256 and settle. com_task, from sdc.c, outside sdc_lock.
 *
 *  want is the SHA-256 the file's footer names. A content with another one is
 *  damaged: the settle then decides no game at all. Returns true for a damaged one. */
bool game_rom_end(const unsigned char want[32]);
/** @brief The ROM file was not sent, it is from before the footer (sdc_image_open, menu_task). No FatFs and no settle here.
 *
 *  Before the game is decided the core has no ROM then, and the settle decides no
 *  game instead of the board's. Afterwards the core keeps the ROM it has, and so
 *  does the game. */
void game_rom_rejected(void);
/** @brief The ROM image is ejected in the OSD (sdc_image_open, menu_task). No FatFs and no settle here; only the ROM block bit moves, the next settle applies "no ROM".
 *
 *  Not for a file that replaces the ROM, whether the core accepts it or not: the
 *  core keeps the ROM it has until a new stream starts, and game_rom_start
 *  takes over then. */
void game_rom_gone(void);
/** @brief True from game_rom_start() until game_rom_end() or game_rom_gone() (an eject). Any task.
 *
 *  A replacement in the OSD closes the old stream with neither, so the flag stays
 *  up until the new stream starts, and the main loop does not settle by "no ROM"
 *  between the close of the old image and the open of the new one. */
bool game_rom_pending(void);
/** @brief True while a ROM streams that began less than limit_ms ago. Any task.
 *
 *  A core switch waits for such a stream, so its settle decides the target and the
 *  newest pick wins. Each stream gets its own limit, a stalled one is not waited for
 *  beyond it. */
bool game_rom_streaming(unsigned limit_ms);

/** @brief The board id from a valid RAM mirror header, byte 12. com_task, whenever one is adopted. */
void game_set_board(unsigned char board);

/** @brief Decides the game from the ROM in the core. com_task only.
 *
 *  Called when board and ROM are known: from game_rom_end(), else at the start of a core and from
 *  main.c when nothing streams or the stream stalled. The first call selects the game from board,
 *  digest and image name and tells game_events_t.first.
 *
 *  Later calls (a ROM picked in the OSD or on the Games page): a known ROM of another game of this
 *  board, proven by its digest, becomes the game in place, see game_new(). Any other file changes
 *  the ROM label of the Version dialog and tells game_events_t.rom_checked. After a core switch
 *  the first settle of the new core makes its game the running one, see game_new_core(). */
void game_settle(void);
/** @brief True once game_settle() ran. com_task. */
bool game_settled(void);

/** @brief Makes another game of this board the running one, from a later settle. com_task only.
 *
 *  Counts the generation and changes the identity whole, then tells game_events_t.replaced. */
void game_new(const ra_ident_t *ident);

/** @brief A core of another board came up without a restart of the Pico. com_task only, after the board of the new core was fed (game_set_board()).
 *
 *  The game of the old core ends with it: a ROM stream cut by the switch is
 *  dropped, and game_new() makes "no game" the running one, so rcheevos
 *  starts anew and the RA task waits. The next settle then makes the game of the
 *  new core's ROM the running one by whatever the table says about it, as the first
 *  settle at the start does, an unknown ROM with a fallback identity included. */
void game_new_core(void);
/** @brief True from game_new_core() until the next settle. com_task. */
bool game_new_core_pending(void);

/** @brief The game of another board a core switch should start, NULL when none. Any task.
 *
 *  Set by game_pick_other_board(): game_switch.c switches the core and starts the game
 *  there. The newest pick wins: a later pick replaces the target, any file that
 *  streams to this core takes it back, until game_switch_commit() fixes it. A
 *  known ROM of another game of this board needs no core switch, the settle makes
 *  it the game in place. */
const ra_game_t *game_switch_to(void);

/** @brief A ROM file picked in the menu: true when it is the card file of a table
 *  game of another board than the running core's. Menu task.
 *
 *  That game becomes game_switch_to(), in place of a pending one, and nothing
 *  streams to this core. While a switch is committed it is dropped instead, the
 *  switch under way comes first. False, and nothing changes, when the board is not
 *  known or the file is no table game's or one of this board: it streams as usual. */
bool game_pick_other_board(const char *name);

/** @brief Fixes the target g right before the marker is written, under the card lock. com_task.
 *
 *  From here picks no longer change it, until game_switch_cancel() runs after
 *  the switch, or the Pico restarts. False when the target is no longer g: a pick
 *  changed it in this moment, and the switch starts over with the new one. */
bool game_switch_commit(const ra_game_t *g);

/** @brief Drops the switch target and its commit: the core switch is done or did not happen. Any task.
 *
 *  The pick of another board is gone with it, a settle after it decides again from
 *  the ROM in the core. */
void game_switch_cancel(void);

/** @brief The id of the running game in the table, false when generation gen no longer runs. Any task. */
bool game_table_id(unsigned gen, unsigned *id);
/** @brief The id the server gave a fallback identity, false when generation gen no longer runs. Any task. */
bool game_resolve(unsigned gen, unsigned id);

/** @brief What a module that keeps something of a game hears of. */
typedef struct {
  void (*rom_unsure)(void); /**< a ROM streams or went, nothing proven. May be menu_task, no FatFs */
  void (*rom_checked)(bool same, const ra_ident_t *ident); /**< a later settle, the game stays. com_task */
  void (*first)(const ra_ident_t *ident);                  /**< the first settle. com_task */
  void (*replaced)(const ra_ident_t *ident);               /**< another game runs. com_task */
} game_events_t;

/** @brief Who hears of the game, one listener. Before the first ROM streams. */
void game_events_set(const game_events_t *e);

#endif /* GAME_H */
