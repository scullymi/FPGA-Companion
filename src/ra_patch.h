/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_patch.h
 *  @brief The current game and its achievement set, read from the card, see ra_patch.c. */
#ifndef RA_PATCH_H
#define RA_PATCH_H

#include <stdbool.h>
#include "rc_runtime.h"
#include "ra_games.h"

/* The identity of the running game, decided by ra_patch_settle() at the start and
   again by ra_patch_new_game() for another game of this board, both in com_task.
   Before settle the accessors say "no game". Each new game counts a generation:
   a task that works for a game over time takes a copy with ra_game_now() and drops
   what it gets back once ra_patch_current() says the game moved on. */

/** @brief The identity of the running game as one copy, see ra_game_now(). */
typedef struct {
  unsigned gen;                           /**< the generation it belongs to, see ra_patch_generation() */
  unsigned id;                            /**< as ra_game_id() */
  char     hash[RA_GAMES_HASH_LEN + 1];   /**< as ra_game_hash() */
} ra_game_now_t;

/** @brief Copies the running game's identity, one game's whole. Any task. */
void ra_game_now(ra_game_now_t *out);
/** @brief Counts the games of this start: 0 for the one settled first, one more per ra_patch_new_game(). Any task. */
unsigned ra_patch_generation(void);
/** @brief True while the game of generation gen runs. Any task. */
bool ra_patch_current(unsigned gen);

/** @brief Hash the server knows the current game by, an md5 in hex. "" when there is no game.
 *
 *  com_task, which alone changes the identity. Other tasks take ra_game_now(): a
 *  new game rewrites the text in place. */
const char *ra_game_hash(void);
/** @brief Id of the current game on the server. 0 when there is none, or the server has not resolved it yet. Any task. */
unsigned    ra_game_id(void);
/** @brief The DIP switches the set expects, n of them. NULL when it expects none, or there is no game. */
const ra_dip_t *ra_game_dips(unsigned *n);
/** @brief Title of the table entry, for the menu. NULL for a fallback identity or before settle. */
const char *ra_game_title(void);
/** @brief Board id of the running core, header byte 12 of the RAM mirror. 0 until a header was read. */
unsigned char ra_game_board(void);
/** @brief What the ROM file is: the known file's label, "ROM unknown", "ROM not checked" (the stream had not ended at settle), "no ROM". "" before settle. */
const char *ra_game_rom_label(void);

#define RA_PATCH_ROM_IMAGE 0   /**< the core's image index of the ROM set, "ROM set" in the core's XML */

/* The ROM hooks run in the task that drives the transfer: rom_data and rom_end in
   com_task, rom_start and rom_gone also in menu_task (a file picked, ejected or
   replaced in the OSD) and under sdc_lock. So rom_start and rom_gone do no FatFs
   and no settle: they record what they know and move the ROM block bit, which can
   log a switch to softcore but never resets the core (a block that appears never
   does). The settle that reads what they stored runs in com_task alone. */

/** @brief The ROM image starts to stream to the core, from sdc.c. No FatFs and no settle here, it may hold sdc_lock; only the ROM block bit moves.
 *
 *  name is the file's name for RetroAchievements' arcade rule (NULL: unknown).
 *  Hardcore is off until ra_patch_rom_end() has checked the whole file. */
void ra_patch_rom_start(const char *name);
/** @brief One block of the ROM image as it goes to the core. com_task, from sdc.c. */
void ra_patch_rom_data(const void *data, unsigned len);
/** @brief The whole ROM image went to the core: finish the SHA-256 and settle. com_task, from sdc.c, outside sdc_lock.
 *
 *  want is the SHA-256 the file's footer names. A content with another one is
 *  damaged: the settle then decides no game at all. Returns true for a damaged one. */
bool ra_patch_rom_end(const unsigned char want[32]);
/** @brief The ROM file was not sent, it is from before the footer (sdc_image_open, menu_task). No FatFs and no settle here.
 *
 *  Before the game is decided the core has no ROM then, and the settle decides no
 *  game instead of the board's. Afterwards the core keeps the ROM it has, and so
 *  does the game. */
void ra_patch_rom_rejected(void);
/** @brief The ROM image is ejected in the OSD (sdc_image_open, menu_task). No FatFs and no settle here; only the ROM block bit moves, the next settle applies "no ROM".
 *
 *  Not for a file that replaces the ROM, whether the core accepts it or not: the
 *  core keeps the ROM it has until a new stream starts, and ra_patch_rom_start
 *  takes over then. */
void ra_patch_rom_gone(void);
/** @brief True from ra_patch_rom_start() until ra_patch_rom_end() or ra_patch_rom_gone() (an eject). Any task.
 *
 *  A replacement in the OSD closes the old stream with neither, so the flag stays
 *  up until the new stream starts, and the main loop does not settle by "no ROM"
 *  between the close of the old image and the open of the new one. */
bool ra_patch_rom_pending(void);
/** @brief True while a ROM streams that began less than limit_ms ago. Any task.
 *
 *  A core switch waits for such a stream, so its settle decides the target and the
 *  newest pick wins. Each stream gets its own limit, a stalled one is not waited for
 *  beyond it. */
bool ra_patch_rom_streaming(unsigned limit_ms);

/** @brief The board id from a valid RAM mirror header, byte 12. com_task, whenever one is adopted. */
void ra_patch_board(unsigned char board);

/** @brief Decides the game from the ROM in the core and reads its card set. com_task only.
 *
 *  Called when board and ROM are known: from ra_patch_rom_end(), or from main.c
 *  when nothing streams or the stream stalled. The first call selects the game
 *  from board, digest and image name, sets RA_HC_BLOCK_ROM and RA_HC_BLOCK_GAME,
 *  moves the card files of the firmware before this one into the game's folder,
 *  and reads the card set: before the RA task is started from the main loop, so
 *  the two never share the set buffer. A set with a valid tag lifts
 *  RA_HC_BLOCK_SET right away, and the game starts in hardcore.
 *
 *  Later calls (a ROM picked in the OSD or on the Games page): a known ROM of
 *  another game of this board, proven by its digest, becomes the game in place,
 *  see ra_patch_new_game(). Any other file moves the two block bits and the ROM
 *  label of the Version dialog, and when it is not the game's ROM it switches the
 *  set off (ra_patch_foreign_rom()) until the game's ROM is back in the core.
 *  After a core switch the first settle of the new core makes its game the
 *  running one, see ra_patch_new_core(). */
void ra_patch_settle(void);
/** @brief True once ra_patch_settle() ran. com_task. */
bool ra_patch_settled(void);

/** @brief Makes another game of this board the running one, from a later settle. com_task only, while the core waits in reset for the new ROM.
 *
 *  Counts the generation and changes the identity whole, then empties what
 *  belonged to the old game: the set table, its texts, leaderboards and rich
 *  presence, the server's warning, the card fingerprint, the foreign ROM state and
 *  a set handed over and not yet taken. RA_HC_BLOCK_ROM, RA_HC_BLOCK_SET and
 *  RA_HC_BLOCK_GAME are set again and lifted as at the start: ROM and GAME here by
 *  ident as the first settle does, SET by the card set with a valid tag or the
 *  server's set. RA_HC_BLOCK_SIZE goes, the new set decides it. The RA task reads
 *  the card set, it owns the set buffer, and begins a round for the new game.
 *  main.c makes rcheevos anew before the next frame, see ra_patch_generation(). */
void ra_patch_new_game(const ra_ident_t *ident);

/** @brief A core of another board came up without a restart of the Pico. com_task only, after the board of the new core was fed (ra_patch_board()).
 *
 *  The game of the old core ends with it: a ROM stream cut by the switch is
 *  dropped, and ra_patch_new_game() makes "no game" the running one, so rcheevos
 *  starts anew and the RA task waits. The next settle then makes the game of the
 *  new core's ROM the running one by whatever the table says about it, as the first
 *  settle at the start does, an unknown ROM with a fallback identity included. */
void ra_patch_new_core(void);
/** @brief True from ra_patch_new_core() until the next settle. com_task. */
bool ra_patch_new_core_pending(void);

/** @brief True while the ROM in the core is another game's than the running one, from a later settle. com_task.
 *
 *  ra_patch_apply_pending() takes the set out of rcheevos meanwhile, ra_queue_add()
 *  drops what would still come. */
bool ra_patch_foreign_rom(void);

/** @brief The game of another board a core switch should start, NULL when none. Any task.
 *
 *  Set by ra_patch_pick_other_board(): main.c switches the core and starts the game
 *  there. The newest pick wins: a later pick replaces the target, any file that
 *  streams to this core takes it back, until ra_patch_restart_commit() fixes it. A
 *  known ROM of another game of this board needs no core switch, the settle makes
 *  it the game in place. */
const ra_game_t *ra_patch_restart_to(void);

/** @brief A ROM file picked in the menu: true when it is the card file of a table
 *  game of another board than the running core's. Menu task.
 *
 *  That game becomes ra_patch_restart_to(), in place of a pending one, and nothing
 *  streams to this core. While a switch is committed it is dropped instead, the
 *  switch under way comes first. False, and nothing changes, when the board is not
 *  known or the file is no table game's or one of this board: it streams as usual. */
bool ra_patch_pick_other_board(const char *name);

/** @brief Fixes the target g right before the marker is written, under the card lock. com_task.
 *
 *  From here picks no longer change it, until ra_patch_restart_cancel() runs after
 *  the switch, or the Pico restarts. False when the target is no longer g: a pick
 *  changed it in this moment, and the switch starts over with the new one. */
bool ra_patch_restart_commit(const ra_game_t *g);

/** @brief Drops the restart target and its commit: the core switch is done or did not happen. Any task.
 *
 *  The pick of another board is gone with it, a settle after it decides again from
 *  the ROM in the core. */
void ra_patch_restart_cancel(void);


/** @brief The id the server resolved the hash of the game of generation gen to. RA task, after r=gameid.
 *
 *  Nothing happens once that game no longer runs. For a table entry a differing
 *  id raises RA_HC_BLOCK_GAME, the table's id is kept. For a fallback identity the
 *  id is stored and its card folder made: on a board the table knows only when it
 *  is one of that board's table games, on a board without a table entry whatever
 *  the server says. With the board still unknown (no RAM mirror header read)
 *  nothing is played. Anything else, and 0 (the server does not know the hash),
 *  leaves no game. */
void ra_patch_resolved(unsigned gen, unsigned server_id);

/** @brief Sets up the handover of a parsed set to com_task. Once, before the RA task runs. False when it failed. */
bool ra_patch_init(void);

#define RA_PATCH_RESET   1u   /**< ra_patch_apply_pending(): the core was reset, a game starts */
#define RA_PATCH_NEW_SET 2u   /**< ra_patch_apply_pending(): a new set is active, table positions changed */

/** @brief Activates a set the RA task handed over, if one waits. com_task only, before each frame.
 *
 *  A set handed over for a game that no longer runs is dropped. Achievements of
 *  the previous set that the new one does not carry are deactivated.
 *  After ra_patch_core_reset() it first resets rcheevos. Returns RA_PATCH_RESET and
 *  RA_PATCH_NEW_SET for what happened, 0 when nothing did. */
unsigned ra_patch_apply_pending(rc_runtime_t *rt);

/** @brief The core has been reset. Any task, it only sets a flag.
 *
 *  Before the next frame, rcheevos starts over: hit counts, leaderboards and rich
 *  presence begin anew, so nothing of the previous game carries into the next. */
void ra_patch_core_reset(void);

/** @brief Reads the card set once more and hands it over, when com_task asked for it. RA task only, never while a set fetch is in flight.
 *
 *  com_task asks (and wakes the task, ra_task_wake()) when the game's ROM is back
 *  after another game's file, see ra_patch_foreign_rom(), and for a new game, see
 *  ra_patch_new_game(). The set is the running game's at the time of the read.
 *  Nothing happens otherwise, so it is cheap to call at every event. True when nothing was asked
 *  for or the card gave a set with a valid tag; false when it did not (no file, no
 *  tag, unusable), the caller then fetches the set from the server again. */
bool ra_patch_set_again(void);

/** @brief The buffer for a set from the server, and its size. RA task only. */
char *ra_patch_body(unsigned *cap);

/** @brief Takes the server's reply of len bytes in that buffer, asked for the game of generation gen. RA task only.
 *
 *  Undoes the chunked framing, parses the set, keeps it on the card when it
 *  differs from the set the card had, and hands it to com_task. Returns 1 for a
 *  new set, 0 when the card already had this one or the game no longer runs (the
 *  reply is dropped), -1 when the reply is no set. A game that moves on while this
 *  runs gets nothing of it: the card file goes to the folder of the game it was
 *  asked for, the hardcore block and the handover check the generation. */
int ra_patch_from_server(unsigned len, unsigned gen);

#define RA_PATCH_TITLE_MAX    32   /**< titles are cut to 31 characters, the FPGA banner shows 24 */
#define RA_PATCH_DESC_MAX     256  /**< descriptions up to 255 characters, as the server allows them */
#define RA_PATCH_PROGRESS_MAX 24   /**< measured progress, "4294967295/4294967295" fits */

/** @brief One achievement of the active set, as the menu shows it. */
typedef struct {
  unsigned id;                              /**< achievement id on the server */
  unsigned points;                          /**< its points */
  char     title[RA_PATCH_TITLE_MAX];        /**< title, cut to fit */
  char     desc[RA_PATCH_DESC_MAX];          /**< description, cut to fit */
  char     progress[RA_PATCH_PROGRESS_MAX];  /**< measured progress, "" when it has none */
  bool     primed;                          /**< challenge on: all its conditions but the trigger hold */
} ra_patch_item_t;

/** @brief Copies achievement i (0-based, in the set's order) to out. Any task.
 *
 *  False when there is no such achievement. */
bool ra_patch_item(unsigned i, ra_patch_item_t *out);

/** @brief The progress of an achievement as text, "12/50" or "37%", "" when there is none. com_task only.
 *
 *  Empty also while the value is unknown or 0, e.g. right after a reset, and below
 *  1 percent, as rc_client shows it. */
void ra_patch_format_progress(const rc_runtime_t *rt, unsigned id, char *buf, size_t size);

/** @brief Reads progress and challenge state of every achievement from rcheevos. com_task only, after a frame.
 *
 *  About once a second is enough, the menu shows what it read last. */
void ra_patch_update_progress(const rc_runtime_t *rt);

#define RA_PATCH_MAX    128  /**< achievements kept per set, e.g. Galaga 17, Pang 114 */
#define RA_PATCH_LB_MAX 12   /**< leaderboards kept per set, e.g. Galaga 1, Ms. Pac-Man 10 */

/** @brief One leaderboard of the active set. */
typedef struct {
  unsigned id;                          /**< leaderboard id on the server */
  int      format;                      /**< how its value reads, for rc_runtime_format_lboard_value() */
  bool     lower_is_better;             /**< e.g. a time */
  char     title[RA_PATCH_TITLE_MAX];    /**< title, cut to fit */
} ra_patch_lboard_t;

/** @brief Copies the leaderboard with this id to out. Any task. False when the set has none such. */
bool ra_patch_lboard(unsigned id, ra_patch_lboard_t *out);

/** @brief The server's warning about this client, e.g. "Unknown Emulator". Any task.
 *
 *  RetroAchievements adds it to the set as an achievement while it has not
 *  fully approved the client for hardcore, e.g. an unknown or outdated one. The
 *  server then records hardcore unlocks as casual and keeps no leaderboard
 *  entries, whatever mode the device is in. It may warn an approved client with
 *  a known issue as well, which this firmware does not tell apart: such a
 *  client's results would read as not recorded. Copies its title without
 *  "Warning: " to out, which may be NULL, and returns true when the set parsed
 *  last, from the card or the server, has one. */
bool ra_patch_warning(char *out, size_t size);

/** @brief Number of core achievements in the active set. */
unsigned    ra_patch_count(void);
/** @brief 1-based position of an achievement id in the set, 0 when not in it. */
unsigned    ra_patch_index(unsigned id);
/** @brief Title of an achievement, "" when unknown. */
const char *ra_patch_title(unsigned id);

#endif /* RA_PATCH_H */
