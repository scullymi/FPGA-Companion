/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file game_switch.c
 *  @brief The way to another game: the core switch over the flash ring, the marker, the ROM of the game picked. */
#include <stdio.h>

#include "../mcu_hw.h"
#include "../debug.h"
#include "../ftpd.h" /* the uploads a restart would cut, for the log */
#include "../menu.h"
#include "../sdc.h"
#include "../sysctrl.h"
#include "banner.h"
#include "core_boot.h"
#include "game_switch.h"
#include "games.h" /* the Games page's pick and the start game */
#include "ra_games.h"
#include "game.h"
#include "ra.h"
#include "ram_mirror.h"

/* A game of another board, e.g. galaga.rom picked in the Pac-Man core
   (game_pick_other_board()), needs its core. The menu says so for a few
   seconds, and a ROM picked meanwhile ends its stream (each within 10 s of its
   start). Unlocks, leaderboard results and FTP uploads go on across the switch.
   The game's id goes into the four marker words of mcu_hw.h, which a restart of
   the MCU keeps. The core pulls RECONFIG_N
   (Z = 0xA5, see game20k_top.sv) and the FPGA loads the next core of the flash
   ring (fpga/common/slots.txt). ring_hops() goes on to the board wanted and
   core_up() sets the core up as the start does: menu, ini, the game's ROM. WiFi,
   the RetroAchievements login, FTP and telnet stay. switch_rom() loads that
   game's ROM instead of image0 of the ini, once: power off brings back the saved
   game, "Save settings" keeps the new one, as before. The newest pick wins until
   the marker is written, see game_switch_to(). Power on loads the first core
   of the ring again.
   The Pico restarts instead when the menu task does not stop in time, and when a
   core of the way does not come up: then it waits for the core at the start as
   after power on, marker_take() moves the marker out at the very start, and
   ring_hops() goes on from there.
   A game picked on the Games page goes the same way, as a file: magic SWITCH_FILE,
   the CRC-32 of its file name instead of the id, and its board with the hops, so a
   game without a row in the table (no achievement set) starts as well.
   A game of the running core's board needs no core switch, see play_here(). A known
   ROM of another game of this board picked under "ROM set", e.g. puckman.rom in
   the Pac-Man core, never comes here: its stream is all it takes, and the settle
   at its end makes it the game (game_new()). */
#define SWITCH_MAGIC   0x67323072u /* "g20r" */
#define SWITCH_FILE    0x67323066u /* "g20f" */
#define SWITCH_SHOW_MS 3000        /* the menu's message first */
#define SWITCH_WAIT_MS 10000       /* then at most this for a ROM stream to end */
#define SWITCH_HOPS    7           /* the 8 MB flash holds at most 8 cores of 1 MB */
/* game20k: the hops word in marker word 3: the switches so far in bits 0 to 7
   (a file's board above them in 8 to 15), the board the pick was made in in 16 to
   23, and HOPS_BACK once the ring had no core of the game's board and the way goes
   back to that board */
#define HOPS_COUNT(w)  ((w) & 0xffu)
#define HOPS_ORIGIN(w) ((unsigned char)((w) >> 16))
#define HOPS_BACK      (1u << 24)

/* The game for the next start, by its row in ra_games.c: a game and its regional sets
   share their RetroAchievements id (gng and makaimurg), the row tells them apart.
   hops counts the core switches on the way to its board, the check covers it too. */
static void switch_mark(const ra_game_t *g, uint32_t hops) {
  uint32_t row = ra_games_row(g);
  mcu_hw_marker_set(0, SWITCH_MAGIC);
  mcu_hw_marker_set(1, row);
  mcu_hw_marker_set(3, hops);
  mcu_hw_marker_set(2, ~(SWITCH_MAGIC ^ row ^ hops));
}

/* A file for the next start, by the CRC-32 of its name in the card's root, with
   the board its core has in marker word 3 above the hops. */
static void switch_mark_file(uint32_t crc, unsigned char board, uint32_t hops) {
  uint32_t b = hops | (uint32_t)board << 8;
  mcu_hw_marker_set(0, SWITCH_FILE);
  mcu_hw_marker_set(1, crc);
  mcu_hw_marker_set(3, b);
  mcu_hw_marker_set(2, ~(SWITCH_FILE ^ crc ^ b));
}

/* S1 and S2 sit on the FPGA's MODE pins (MODE0 and MODE1, schematic 3923). One
   held while the FPGA reloads selects another way of loading, and no core comes up
   until power off and on. The switch waits until both are released, read under
   the card lock right before Z. The read also re-arms the button interrupt, so a
   press in that moment may not reach the menu, the core is about to go anyway. */
static bool buttons_held(void) { return (sys_get_buttons() & 3) != 0; }

/* The core pulls RECONFIG_N and the FPGA loads the next core of the ring. Caller
   holds sdc_lock: the card hangs on the FPGA, no card operation may run while it
   goes. Caller has seen S1 and S2 released, see buttons_held(). False when the
   core still answers 50 ms later: it does not know Z, it was built before the
   switch, and goes on. A new core is not up that early, a full bitstream takes
   0.3 s from the flash at 25 MHz. */
static bool core_switch(void) {
  sys_set_val('Z', (int8_t)0xA5);
  vTaskDelay(pdMS_TO_TICKS(50));
  return !sys_status_is_valid();
}

/* game20k: a game of the running core's board, picked on the Games page, streams in
   place: its ROM goes to the core as a pick under "ROM set" does, the core waits in
   reset until it is whole, and the settle at its end makes it the game, a known ROM
   of another game with its own set, session and card folder (game_new()).
   The pick is fixed before its file is looked up, so a newer one waits for the next
   pass, and dropped once the stream runs: from then on the stream is the newest pick. */
static void play_here(bool file, const games_pick_t *pk, const ra_game_t *g) {
  const int drive = MAX_DRIVES + GAME_ROM_IMAGE;
  char path[FF_LFN_BUF + 6], name[FF_LFN_BUF + 1]; /* "/sd/" plus a file name, and the name */
  if(!(file ? games_pick_commit(pk->seq) : game_switch_commit(g))) return;
  bool found = file ? games_find(pk->crc, path, sizeof(path))
                    : snprintf(path, sizeof(path), "%s/%s.rom", CARD_MOUNTPOINT, g->set) < (int)sizeof(path);
  int r      = 1;
  if(found) {
    // cwd and name as switch_rom() sets them for a start, "Save settings" keeps them.
    // sdc_image_open() frees the name it holds, so it gets a copy
    sdc_set_default(drive, path);
    snprintf(name, sizeof(name), "%s", sdc_get_image_name(drive) ? sdc_get_image_name(drive) : "");
    r = name[0] ? sdc_image_open(drive, name) : 1;
  }
  if(file) games_pick_cancel();
  else game_switch_cancel();
  if(r)
    debugf("%s: %s not streamed (%d), the game goes on", file ? "Games" : "RA", found ? path : "the file", r);
  else debugf("%s: %s streams to the core, no restart", file ? "Games" : "RA", path);
}

static bool core_up(void);

/* Returns true when another core came up without a restart: the poll that called
   it ends, the next one reads the new core. */
bool switch_step(void) {
  static TickType_t start;
  static uintptr_t shown; /* the target the message is for, 0 for none */
  // a pick on the Games page goes first, else the game a ROM picked under "ROM set" asks for
  games_pick_t pk;
  bool file          = games_picked(&pk);
  const ra_game_t *g = file ? NULL : game_switch_to();
  if(!file && !g) { /* none, or the ROM picked last took it back */
    start = 0;
    shown = 0;
    return false;
  }
  // a pick is told apart by its count, odd so it never equals an entry's address
  uintptr_t key       = file ? ((uintptr_t)pk.seq << 1) | 1 : (uintptr_t)g;
  unsigned char board = file ? pk.board : g->board;
  const char *title   = file ? pk.title : g->title;
  TickType_t now      = xTaskGetTickCount();
  // game20k: this board, no core switch. A message for another target ends with it
  if(board == ram_mirror_board) {
    start = 0;
    shown = 0;
    play_here(file, &pk, g);
    return false;
  }
  if(!start || key != shown) { /* a new target: its message first */
    start = now ? now : 1;
    shown = key;
    menu_notify(MENU_EVENT_CORE_SWITCH);
    return false;
  }
  if(now - start < pdMS_TO_TICKS(SWITCH_SHOW_MS)) return false;
  // a ROM picked last streams until its settle decides the target. Unlocks,
  // leaderboard results and FTP uploads go on across the switch
  if(game_rom_streaming(SWITCH_WAIT_MS)) /* its own limit: also a pick made late wins */
    return false;
  if(buttons_held()) return false; /* read again under the lock, this spares the menu a stop */
  // game20k: the menu task stops before the card lock, it may need the card to end
  // what it does, and from then on no pick and no menu of the old core runs. A
  // menu task that does not stop in time leaves the way with a restart
  bool parked = menu_park();
  // the last checks under the card lock, where they hold until the switch: every
  // card operation ends whole, a ROM starts to stream only under the lock, S1 and
  // S2 are read right before Z and not before a wait for the lock. The target is
  // fixed last, a pick can change it until then
  sdc_lock();
  bool held = board != ram_mirror_board && buttons_held();
  if(held || game_rom_streaming(SWITCH_WAIT_MS) ||
     !(file ? games_pick_commit(pk.seq) : game_switch_commit(g))) {
    sdc_unlock();
    if(parked) menu_resume();
    return false;
  }
  if(file) switch_mark_file(pk.crc, board, (uint32_t)ram_mirror_board << 16);
  else switch_mark(g, (uint32_t)ram_mirror_board << 16);
  start = 0;
  shown = 0;
  if(parked) {
    // the way of ring_hops() from the running core, the marker words
    // carry it on if a core does not come up
    sdc_unlock();
    debugf("Core switch for %s, board %u, without a restart", title, board);
    if(core_up()) {
      if(file) games_pick_cancel();
      else game_switch_cancel();
      return true;
    }
  } else {
    // the restart cuts what is not on the card yet
    if(ftpd_uploads() || ra_queue_in_transit() || ra_task_lboard_pending())
      debugf(
          "Core switch: the restart cuts %u FTP upload(s), %u unlocks and %u leaderboard results not yet out",
          ftpd_uploads(), ra_queue_in_transit(), ra_task_lboard_pending());
    debugf("Core switch for %s, board %u, with a restart", title, board);
    if(core_switch()) mcu_hw_reset();
    // the bitstream has no switch: the game goes on, and the menu says why
    mcu_hw_marker_set(0, 0);
    sdc_unlock();
  }
  debugf("Core switch: the core did not reload, its bitstream does not know Z");
  if(parked) menu_resume();
  if(file) games_pick_cancel();
  else game_switch_cancel();
  menu_notify(MENU_EVENT_CORE_SWITCH_FAILED);
  // the ROM now in the core decides again: a known ROM of another game of this
  // board, streamed while the switch was the target, becomes the game. A stream
  // that still runs settles by itself when it ends
  if(!game_rom_pending()) game_settle();
  return false;
}

/* The marker as the Pico found it at its start. marker_take() moves it out of
   the marker words before the wait for the FPGA: a boot that never reaches
   switch_rom(), because no core comes up, must not leave it for the next core
   that shows up, it would switch that one away. */
static uint32_t marker[4];

void marker_take(void) {
  for(int i = 0; i < 4; i++) marker[i] = mcu_hw_marker_get(i);
  mcu_hw_marker_set(0, 0);
}

/* At power-on, when no restart left a marker: the start game Save settings keeps
   (games.c) becomes the marker switch_rom() follows, a core switch first when its
   board is not the core that came up. Once per power-on: every restart of the MCU
   is one mcu_hw_restarted() tells, and a missing or old start game leaves the ini's ROM. */
void start_game(void) {
  char name[FF_LFN_BUF + 1];
  games_footer_t ft;
  if((marker[0] == SWITCH_MAGIC || marker[0] == SWITCH_FILE) &&
     marker[2] == ~(marker[0] ^ marker[1] ^ marker[3]))
    return;
  if(mcu_hw_restarted() || !games_start_file(name, sizeof(name), &ft)) return;
  debugf("Games: start game %s, board %u", name, ft.board);
  uint32_t crc = games_name_crc(name), b = (uint32_t)ft.board << 8;
  marker[0] = SWITCH_FILE;
  marker[1] = crc;
  marker[3] = b;
  marker[2] = ~(SWITCH_FILE ^ crc ^ b);
}

/* Before the images are mounted, at the start and after a core switch: the game
   the switch was for, once ring_hops() reached its board. That board loads its ROM
   instead of image0 of the ini, any other board keeps the ini's ROM. */
void switch_rom(void) {
  uint32_t magic = marker[0], id = marker[1], check = marker[2], hops = marker[3];
  const ra_game_t *g = NULL;
  unsigned char board;
  const char *title;
  if(check != ~(magic ^ id ^ hops)) return;
  if(magic == SWITCH_MAGIC) {
    // id is the game's row in the table, several rows share an RA id
    if(!(g = ra_games_at(id))) return;
    board = g->board;
    title = g->title;
  } else if(magic == SWITCH_FILE) {
    // id is the CRC of the file name, the board rides above the hops
    board = (unsigned char)(hops >> 8);
    hops &= ~0xff00u;
    title = "the game picked";
  } else return;
  // the start game at power on was picked in the core that came up first
  if(!HOPS_ORIGIN(hops) && !HOPS_COUNT(hops)) hops |= (uint32_t)ram_mirror_board << 16;
  bool back            = hops & HOPS_BACK;
  unsigned char target = back ? HOPS_ORIGIN(hops) : board;
  if(target != ram_mirror_board) {
    // ring_hops() went as far as the way goes: no valid header, a bitstream without
    // Z, or the whole ring without the game's board
    debugf("Core switch: no core of board %u for %s, the ini's ROM", target, title);
    return;
  }
  if(back) {
    // back in the core the game was picked in, with the ini's ROM: the flash has
    // no core of the game's board (fewer cores flashed than slots.txt lists)
    debugf("Core switch: back at board %u, the flash has no core of board %u", target, board);
    banner_show_kept("SYS: CORE NOT IN FLASH", false, false);
    return;
  }
  char path[FF_LFN_BUF + 6]; /* "/sd/" plus a file name */
  if(g) {
    snprintf(path, sizeof(path), "/sd/%s.rom", g->set);
    // the switch went by the file name, the file is taken from the card's root: a
    // file picked in a folder, or one gone since, leaves the ini's ROM
    FILINFO fno;
    sdc_lock();
    FRESULT r = f_stat(path, &fno);
    sdc_unlock();
    if(r != FR_OK) {
      debugf("RA: started for %s, but %s is missing (%d), the ini's ROM", title, path, r);
      return;
    }
    debugf("RA: started for %s, ROM %s instead of the ini's", title, path);
  } else if(!games_find(id, path, sizeof(path))) {
    debugf("Games: the file picked is gone from the card, the ini's ROM");
    return;
  } else debugf("Games: started with %s instead of the ini's ROM", path);
  sdc_set_default(MAX_DRIVES + GAME_ROM_IMAGE, path);
}

/* game20k: a switch to a core of another board hops the ring here, right after the
   first look at the header and before anything of the core is set up (menu,
   settings, ROM): one start of the Pico for the whole way instead of one per hop.
   The way is that of switch_rom(), back to the board of the pick after a whole
   round without the game's board. Each hop pulls RECONFIG_N (Z), waits for the next
   core and its card and reads the new board. The marker words
   follows each hop, so a core that does not come up ends in a restart that goes on
   from there as before. The marker in RAM ends at the board reached, switch_rom()
   then loads the ROM or says why not. probe takes the verdict of the header of
   the last core loaded. True when another core came up.
   A switch while a game runs comes here as well, see switch_step(): the marker
   it wrote is the way, the first hop goes from the running core. */
bool ring_hops(unsigned char *probe) {
  uint32_t magic = marker[0], id = marker[1], hops = marker[3];
  const ra_game_t *g = NULL;
  unsigned char board;
  bool hopped = false;
  if(marker[2] != ~(magic ^ id ^ hops)) return false;
  if(magic == SWITCH_MAGIC) {
    if(!(g = ra_games_at(id))) return false;
    board = g->board;
  } else if(magic == SWITCH_FILE) {
    board = (unsigned char)(hops >> 8);
    hops &= ~0xff00u;
  } else return false;
  const char *title = g ? g->title : "the game picked";
  // a core the way only passes is reloaded before its card is ready, the next core
  // starts the card anew: the card lock stays held from one hop to the next, and only
  // the core the way ends at is waited for, see below the loop
  bool locked = false;
  for(;;) {
    if(!HOPS_ORIGIN(hops) && !HOPS_COUNT(hops)) hops |= (uint32_t)ram_mirror_board << 16;
    bool back            = hops & HOPS_BACK;
    unsigned char target = back ? HOPS_ORIGIN(hops) : board;
    // the board reached, no header, or the end of the way: switch_rom() takes over
    if(!ram_mirror_board || target == ram_mirror_board) break;
    // back at the board of the pick: the whole round had no core of the game's board,
    // and the way back ends right here
    if(!back && HOPS_COUNT(hops) && ram_mirror_board == HOPS_ORIGIN(hops)) {
      debugf("Core switch: no core of board %u for %s in the whole ring", board, title);
      hops = HOPS_BACK | (hops & 0xff0000u);
      break;
    }
    if(HOPS_COUNT(hops) >= SWITCH_HOPS) {
      if(back || !HOPS_ORIGIN(hops)) break;
      debugf("Core switch: no core of board %u for %s, back to board %u", board, title, HOPS_ORIGIN(hops));
      hops   = HOPS_BACK | (hops & 0xff0000u);
      target = HOPS_ORIGIN(hops);
    } else hops++;
    if(g) switch_mark(g, hops);
    else switch_mark_file(id, board, hops);
    for(;;) { /* S1 and S2 read under the lock, right before Z */
      if(!locked) sdc_lock();
      locked = true;
      if(!buttons_held()) break;
      // the wait for the buttons frees the lock: the card of a core passed is made
      // ready first, no other task may find it half set up
      if(hopped) {
        unsigned char status;
        (void)sdc_wait_ready(&status);
      }
      sdc_unlock();
      locked = false;
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    debugf("Core switch for %s, board %u, on from board %u within this start", title, target,
           ram_mirror_board);
    if(!core_switch()) { /* the bitstream does not know Z: switch_rom() says so */
      sdc_unlock();
      mcu_hw_marker_set(0, 0);
      return hopped;
    }
    if(!sys_wait4fpga()) {
      debugf("Core switch: no core after the hop, restart");
      mcu_hw_reset(); /* the marker carries the way on */
    }
    ram_mirror_forget();
    *probe = ram_mirror_probe();
    hopped = true;
  }
  // the core the way ends at: its card, then its cold boot as at the start of the Pico.
  // The interrupts only after the card lock is free, an SDC request takes it
  if(locked) {
    unsigned char status;
    if(sdc_wait_ready(&status)) {
      debugf("Core switch: no card after the hop, restart");
      mcu_hw_reset();
    }
    sdc_unlock();
    sys_handle_interrupts(sys_irq_ctrl(0xff), true);
  }
  marker[3] = magic == SWITCH_FILE ? hops | (uint32_t)board << 8 : hops;
  marker[2] = ~(magic ^ id ^ marker[3]);
  mcu_hw_marker_set(0, 0);
  return hopped;
}

/* game20k: the core switch without a restart of the Pico, from switch_step() with
   the menu task parked and the way in the marker words. ring_hops() loads the
   cores of the way, core_setup() the last one. False when the running bitstream
   does not know Z: nothing changed, the card lock is free again. */
static bool core_up(void) {
  TickType_t t0 = xTaskGetTickCount();
  unsigned char probe;
  for(int i = 0; i < 4; i++) marker[i] = mcu_hw_marker_get(i);
  if(!ring_hops(&probe)) return false;
  core_setup(probe, t0);
  return true;
}

/* game20k: a core loaded from outside while the Pico runs, e.g. by openFPGALoader
   over the BL616 of the Nano. Only the cold boot bit tells it, sysctrl.c asks here
   instead of a reset. The core is up already: once its card is ready it is set up
   as after a core switch, with the ROM of its ini, and a pick that waited for a
   switch is dropped. False when the menu task does not stop in time, the reset
   follows then. No card leaves the way to the start as well, by a reset. */
bool sys_core_changed(void) {
  TickType_t t0 = xTaskGetTickCount();
  unsigned char status;
  if(!menu_park()) return false;
  debugf("Core switch: a core was loaded from outside, set up without a restart");
  sdc_lock();
  if(sdc_wait_ready(&status)) {
    debugf("Core switch: no card on the new core, restart");
    mcu_hw_reset();
  }
  sdc_unlock();
  games_pick_cancel();
  game_switch_cancel();
  marker[0] = 0; /* no game picked: switch_rom() leaves the ini's ROM */
  ram_mirror_forget();
  core_setup(ram_mirror_probe(), t0);
  return true;
}
