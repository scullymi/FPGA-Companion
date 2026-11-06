/*
  main.c - MiSTeryNano FPGA Companion Pi Pico variant

*/

#include "../mcu_hw.h"
uint32_t getFreeHeap(void);   /* mcu_hw.c: the SDK heap left for mbedTLS and rcheevos */
uint32_t getTotalHeap(void);  /* mcu_hw.c: the SDK heap, all of it */
#include <malloc.h>           /* game20k: mallinfo(), the SDK heap's top after a game switch */

#include "../config.h"
#include "../sysctrl.h"
#include "../sdc.h"
#include "../osd.h"
#include "../menu.h"
#include "../inifile.h"
#include "../debug.h"
#include "../spi.h"     /* SPI_TARGET_RAM */
#include "rc_runtime.h"
#include "../game20k/ra_patch.h"
#include "../game20k/ra_queue.h"
#include "../game20k/ra_state.h"
#include "../game20k/ra_task.h"
#include "../game20k/ra_mac.h"
#include <string.h>
#include "pico/time.h"
#include "hardware/watchdog.h"   /* the way of a core switch across a restart, see switch_step() */
#include "../ftpd.h"             /* the uploads a restart would cut, for the log */
#include "../game20k/games.h"            /* the Games page's pick and the start game */
#include "../game20k/banner.h"
#include "../game20k/menus.h"
#include "../game20k/ra_events.h"            /* the menus of game20k's cores */

/* RAM mirror on SPI target 5: each poll reads the header and, if there is a new
   snapshot, fetches the game RAM plus the oracle log, checks it, and sends the verdict
   as the first byte of the next transfer. The seven constants are the same for every
   game and must match ram_mirror_pkg.sv of the game20k FPGA platform. Layout 5: a
   header of 24 bytes, in it the core's reset count (byte 8), its diagnostic
   parameters (byte 9) and their complements (bytes 10 and 11), the board id of the
   core (byte 12, its complement in 13), the game RAM in the mirror in units of
   RAM_MIRROR_PAGE bytes (byte 14, its complement in 15) and the core's interface tag
   (bytes 16 and 17, low byte first, the complements in 18 and 19), which picks the
   menu (menus.h). Bytes 20 to 23 are 0. Each core sends its own length, the buffers
   here hold the largest one. */
#define RAM_MIRROR_LAYOUT    0x05                    /* header byte 4 */
#define RAM_MIRROR_HEAD      24
#define RAM_MIRROR_DATA_MAX  22528                   /* largest game RAM a core may announce */
#define RAM_MIRROR_LOG       1536                    /* oracle log, 512 x 3 bytes */
#define RAM_MIRROR_TAIL      8                       /* footer: frame, flags, checksum, log count */
#define RAM_MIRROR_BYTES_MAX (RAM_MIRROR_HEAD + RAM_MIRROR_DATA_MAX + RAM_MIRROR_LOG + RAM_MIRROR_TAIL)
#define RAM_MIRROR_PAGE      128                     /* unit of header byte 14, 176 pages at most */

static unsigned char ram_mirror_buf[RAM_MIRROR_BYTES_MAX];
static unsigned char ram_mirror_verdict = 0xA5;
static int ram_mirror_frame = -1;                    /* frame number of the last good snapshot */
static int ram_mirror_resets = -1;                   /* the core's reset count in it */
static unsigned char ram_mirror_seen[RAM_MIRROR_DATA_MAX / 8];
/* the running core's game RAM in the mirror, byte 14 x RAM_MIRROR_PAGE, 0 until a
   valid header was read; com_task only */
static unsigned      ram_mirror_data;
static unsigned char ram_mirror_board;               /* its board id, byte 12, adopted with the length */
static uint16_t      ram_mirror_tag;                 /* its interface tag, bytes 16 and 17, adopted with them */

/* A game of another board, e.g. galaga.rom picked in the Pac-Man core
   (ra_patch_pick_other_board()), needs its core. The menu says so for a few
   seconds, and a ROM picked meanwhile ends its stream (each within 10 s of its
   start). Unlocks, leaderboard results and FTP uploads go on across the switch.
   The game's id goes into the watchdog's scratch registers 0 to 3, which the SDK
   leaves to the program and which a reboot keeps. The core pulls RECONFIG_N
   (Z = 0xA5, see game20k_top.sv) and the FPGA loads the next core of the flash
   ring (fpga/common/slots.txt). ring_hops() goes on to the board wanted and
   core_up() sets the core up as the start does: menu, ini, the game's ROM. WiFi,
   the RetroAchievements login, FTP and telnet stay. switch_rom() loads that
   game's ROM instead of image0 of the ini, once: power off brings back the saved
   game, "Save settings" keeps the new one, as before. The newest pick wins until
   the marker is written, see ra_patch_switch_to(). Power on loads the first core
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
   at its end makes it the game (ra_patch_new_game()). */
#define SWITCH_MAGIC   0x67323072u          /* "g20r" */
#define SWITCH_FILE    0x67323066u          /* "g20f" */
#define SWITCH_SHOW_MS 3000                 /* the menu's message first */
#define SWITCH_WAIT_MS 10000                /* then at most this for a ROM stream to end */
#define SWITCH_HOPS     7                    /* the 8 MB flash holds at most 8 cores of 1 MB */
/* game20k: the hops word in scratch register 3: the switches so far in bits 0 to 7
   (a file's board above them in 8 to 15), the board the pick was made in in 16 to
   23, and HOPS_BACK once the ring had no core of the game's board and the way goes
   back to that board */
#define HOPS_COUNT(w)   ((w) & 0xffu)
#define HOPS_ORIGIN(w)  ((unsigned char)((w) >> 16))
#define HOPS_BACK       (1u << 24)

/* The game for the next start, by its row in ra_games.c: a game and its regional sets
   share their RetroAchievements id (gng and makaimurg), the row tells them apart.
   hops counts the core switches on the way to its board, the check covers it too. */
static void switch_mark(const ra_game_t *g, uint32_t hops) {
  uint32_t row = ra_games_row(g);
  watchdog_hw->scratch[0] = SWITCH_MAGIC;
  watchdog_hw->scratch[1] = row;
  watchdog_hw->scratch[3] = hops;
  watchdog_hw->scratch[2] = ~(SWITCH_MAGIC ^ row ^ hops);
}

/* A file for the next start, by the CRC-32 of its name in the card's root, with
   the board its core has in scratch register 3 above the hops. */
static void switch_mark_file(uint32_t crc, unsigned char board, uint32_t hops) {
  uint32_t b = hops | (uint32_t)board << 8;
  watchdog_hw->scratch[0] = SWITCH_FILE;
  watchdog_hw->scratch[1] = crc;
  watchdog_hw->scratch[3] = b;
  watchdog_hw->scratch[2] = ~(SWITCH_FILE ^ crc ^ b);
}

/* S1 and S2 sit on the FPGA's MODE pins (MODE0 and MODE1, schematic 3923). One
   held while the FPGA reloads selects another way of loading, and no core comes up
   until power off and on. The switch waits until both are released, read under
   the card lock right before Z. The read also re-arms the button interrupt, so a
   press in that moment may not reach the menu, the core is about to go anyway. */
static bool buttons_held(void) {
  return (sys_get_buttons() & 3) != 0;
}

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
   of another game with its own set, session and card folder (ra_patch_new_game()).
   The pick is fixed before its file is looked up, so a newer one waits for the next
   pass, and dropped once the stream runs: from then on the stream is the newest pick. */
static void play_here(bool file, const games_pick_t *pk, const ra_game_t *g) {
  const int drive = MAX_DRIVES + RA_PATCH_ROM_IMAGE;
  char path[FF_LFN_BUF + 6], name[FF_LFN_BUF + 1];   /* "/sd/" plus a file name, and the name */
  if(!(file ? games_pick_commit(pk->seq) : ra_patch_switch_commit(g))) return;
  bool found = file ? games_find(pk->crc, path, sizeof(path))
                    : snprintf(path, sizeof(path), "%s/%s.rom", CARD_MOUNTPOINT, g->set) < (int)sizeof(path);
  int r = 1;
  if(found) {
    // cwd and name as switch_rom() sets them for a start, "Save settings" keeps them.
    // sdc_image_open() frees the name it holds, so it gets a copy
    sdc_set_default(drive, path);
    snprintf(name, sizeof(name), "%s", sdc_get_image_name(drive) ? sdc_get_image_name(drive) : "");
    r = name[0] ? sdc_image_open(drive, name) : 1;
  }
  if(file) games_pick_cancel();
  else     ra_patch_switch_cancel();
  if(r) debugf("%s: %s not streamed (%d), the game goes on", file ? "Games" : "RA", found ? path : "the file", r);
  else  debugf("%s: %s streams to the core, no restart", file ? "Games" : "RA", path);
}

static bool core_up(void);

/* Returns true when another core came up without a restart: the poll that called
   it ends, the next one reads the new core. */
static bool switch_step(void) {
  static TickType_t start;
  static uintptr_t  shown;           /* the target the message is for, 0 for none */
  // a pick on the Games page goes first, else the game a ROM picked under "ROM set" asks for
  games_pick_t pk;
  bool file = games_picked(&pk);
  const ra_game_t *g = file ? NULL : ra_patch_switch_to();
  if(!file && !g) {                  /* none, or the ROM picked last took it back */
    start = 0;
    shown = 0;
    return false;
  }
  // a pick is told apart by its count, odd so it never equals an entry's address
  uintptr_t key = file ? ((uintptr_t)pk.seq << 1) | 1 : (uintptr_t)g;
  unsigned char board = file ? pk.board : g->board;
  const char *title = file ? pk.title : g->title;
  TickType_t now = xTaskGetTickCount();
  // game20k: this board, no core switch. A message for another target ends with it
  if(board == ram_mirror_board) {
    start = 0;
    shown = 0;
    play_here(file, &pk, g);
    return false;
  }
  if(!start || key != shown) {       /* a new target: its message first */
    start = now ? now : 1;
    shown = key;
    menu_notify(MENU_EVENT_CORE_SWITCH);
    return false;
  }
  if(now - start < pdMS_TO_TICKS(SWITCH_SHOW_MS)) return false;
  // a ROM picked last streams until its settle decides the target. Unlocks,
  // leaderboard results and FTP uploads go on across the switch
  if(ra_patch_rom_streaming(SWITCH_WAIT_MS))   /* its own limit: also a pick made late wins */
    return false;
  if(buttons_held()) return false;   /* read again under the lock, this spares the menu a stop */
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
  if(held || ra_patch_rom_streaming(SWITCH_WAIT_MS) ||
     !(file ? games_pick_commit(pk.seq) : ra_patch_switch_commit(g))) {
    sdc_unlock();
    if(parked) menu_resume();
    return false;
  }
  if(file) switch_mark_file(pk.crc, board, (uint32_t)ram_mirror_board << 16);
  else     switch_mark(g, (uint32_t)ram_mirror_board << 16);
  start = 0;
  shown = 0;
  if(parked) {
    // the way of ring_hops() from the running core, the marker in the scratch
    // registers carries it on if a core does not come up
    sdc_unlock();
    debugf("Core switch for %s, board %u, without a restart", title, board);
    if(core_up()) {
      if(file) games_pick_cancel();
      else     ra_patch_switch_cancel();
      return true;
    }
  } else {
    // the restart cuts what is not on the card yet
    if(ftpd_uploads() || ra_queue_in_transit() || ra_task_lboard_pending())
      debugf("Core switch: the restart cuts %u FTP upload(s), %u unlocks and %u leaderboard results not yet out",
             ftpd_uploads(), ra_queue_in_transit(), ra_task_lboard_pending());
    debugf("Core switch for %s, board %u, with a restart", title, board);
    if(core_switch()) mcu_hw_reset();
    // the bitstream has no switch: the game goes on, and the menu says why
    watchdog_hw->scratch[0] = 0;
    sdc_unlock();
  }
  debugf("Core switch: the core did not reload, its bitstream does not know Z");
  if(parked) menu_resume();
  if(file) games_pick_cancel();
  else     ra_patch_switch_cancel();
  menu_notify(MENU_EVENT_CORE_SWITCH_FAILED);
  // the ROM now in the core decides again: a known ROM of another game of this
  // board, streamed while the switch was the target, becomes the game. A stream
  // that still runs settles by itself when it ends
  if(!ra_patch_rom_pending()) ra_patch_settle();
  return false;
}

/* The marker as the Pico found it at its start. marker_take() moves it out of
   the scratch registers before the wait for the FPGA: a boot that never reaches
   switch_rom(), because no core comes up, must not leave it for the next core
   that shows up, it would switch that one away. */
static uint32_t marker[4];

static void marker_take(void) {
  for(int i = 0; i < 4; i++) marker[i] = watchdog_hw->scratch[i];
  watchdog_hw->scratch[0] = 0;
}

/* At power-on, when no restart left a marker: the start game Save settings keeps
   (games.c) becomes the marker switch_rom() follows, a core switch first when its
   board is not the core that came up. Once per power-on: every restart of the Pico
   is a watchdog reboot, and a missing or old start game leaves the ini's ROM. */
static void start_game(void) {
  char name[FF_LFN_BUF + 1];
  games_footer_t ft;
  if((marker[0] == SWITCH_MAGIC || marker[0] == SWITCH_FILE) &&
     marker[2] == ~(marker[0] ^ marker[1] ^ marker[3]))
    return;
  if(watchdog_caused_reboot() || !games_start_file(name, sizeof(name), &ft)) return;
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
static void switch_rom(void) {
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
  } else
    return;
  // the start game at power on was picked in the core that came up first
  if(!HOPS_ORIGIN(hops) && !HOPS_COUNT(hops)) hops |= (uint32_t)ram_mirror_board << 16;
  bool back = hops & HOPS_BACK;
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
  char path[FF_LFN_BUF + 6];     /* "/sd/" plus a file name */
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
  } else
    debugf("Games: started with %s instead of the ini's ROM", path);
  sdc_set_default(MAX_DRIVES + RA_PATCH_ROM_IMAGE, path);
}

/* Reads the header of one transfer, full duplex: hdr_tx goes to the FPGA meanwhile.
   Shared by the probe at boot and the poll. Returns 0 with SPI still open and the
   header in ram_mirror_buf, or the verdict with SPI ended: 0xE1 magic, 0xE2 layout
   (core and firmware of different releases, said once per layout seen; the banner
   still reaches an older core, the back channel in bytes 0 to 7 is the same), 0xE3
   bytes 12 to 19 (said once per refused set seen). Those carry the board id, the
   length and the interface tag with a complement each: a pair that does not match,
   a board of 0 or 255 (the stuck patterns 00 and FF fail here) or a length outside
   1..176 pages is refused and no body is read. The first valid set is adopted,
   together with the board id for the game's identity, and a later set that differs
   is refused too: the FPGA cannot change without a reconfiguration, which reboots
   the Pico as well. */
static unsigned char ram_mirror_header(const unsigned char *hdr_tx) {
  mcu_hw_spi_begin();
  mcu_hw_spi_tx_u08(SPI_TARGET_RAM);
  mcu_hw_spi_tx_u08(ram_mirror_verdict);     /* verdict on the previous transfer */
  mcu_hw_spi_txrx_block(hdr_tx, ram_mirror_buf, RAM_MIRROR_HEAD);

  if(ram_mirror_buf[0] != 'R' || ram_mirror_buf[1] != 'A' ||
     ram_mirror_buf[2] != 'C' || ram_mirror_buf[3] != 'H') {
    mcu_hw_spi_end(); return ram_mirror_verdict = 0xE1;
  }
  if(ram_mirror_buf[4] != RAM_MIRROR_LAYOUT) {
    static int told = -1;
    if(told != ram_mirror_buf[4]) {
      told = ram_mirror_buf[4];
      debugf("RAM mirror: the core sends layout %u, this firmware reads %u, "
             "flash core and firmware of the same release", ram_mirror_buf[4], RAM_MIRROR_LAYOUT);
      banner_show_kept(ram_mirror_buf[4] < RAM_MIRROR_LAYOUT ? "SYS: CORE TOO OLD" : "SYS: FIRMWARE TOO OLD",
                  false, false);
    }
    mcu_hw_spi_end(); return ram_mirror_verdict = 0xE2;
  }
  unsigned char board = ram_mirror_buf[12], pages = ram_mirror_buf[14];
  unsigned      data  = (unsigned)pages * RAM_MIRROR_PAGE;
  uint16_t      tag   = (uint16_t)(ram_mirror_buf[16] | ram_mirror_buf[17] << 8);
  bool valid = ram_mirror_buf[13] == (unsigned char)~board && ram_mirror_buf[15] == (unsigned char)~pages &&
               ram_mirror_buf[18] == (unsigned char)~ram_mirror_buf[16] &&
               ram_mirror_buf[19] == (unsigned char)~ram_mirror_buf[17] &&
               board != 0 && board != 255 && pages >= 1 && pages <= RAM_MIRROR_DATA_MAX / RAM_MIRROR_PAGE;
  if(valid && !ram_mirror_data) {
    ram_mirror_data  = data;
    ram_mirror_board = board;
    ram_mirror_tag   = tag;
    ra_patch_board(board);                 /* whichever of probe or poll sees it first feeds the identity */
    debugf("RAM mirror: layout %u, board %u, %u data bytes, interface tag %04x",
           RAM_MIRROR_LAYOUT, board, data, tag);
  } else if(!valid || data != ram_mirror_data || board != ram_mirror_board || tag != ram_mirror_tag) {
    /* once per distinct refused value, as the layout branch above does. A flag of
       its own for "nothing said yet": all 00 and all FF are values this branch
       refuses, so neither can serve as the sentinel */
    static bool     told_any;
    static uint32_t told[2];
    uint32_t cur[2] = { 0, 0 };
    for(int i = 0; i < 8; i++) cur[i / 4] = cur[i / 4] << 8 | ram_mirror_buf[12 + i];
    if(!told_any || told[0] != cur[0] || told[1] != cur[1]) {
      told_any = true;
      told[0] = cur[0];
      told[1] = cur[1];
      debugf("RAM mirror: header bytes 12..19 %08lx %08lx %s, snapshot refused",
             (unsigned long)cur[0], (unsigned long)cur[1],
             valid ? "differ from the adopted board, length and tag" : "invalid");
    }
    mcu_hw_spi_end(); return ram_mirror_verdict = 0xE3;
  }
  return 0;
}

/* Board id and interface tag before the menu and the game: the header only, up to 50
   tries 10 ms apart (half a second), at boot before the menu is read. ram_spi answers
   the header from constants while the core is still in reset. On failure (a core of
   another layout, no answer) the board stays 0, the poll adopts the first valid
   header it sees later, and the game's identity is decided as "board unknown". hdr_tx
   all zero: byte 5 is taken only when two transfers agree, and zero is its boot value.
   Returns 0 for a valid header, else the last verdict: 0xE1 no game20k core, 0xE2 a
   game20k core of another layout, 0xE3 one with a broken header.
   game20k: the probe also takes the core's reset count (byte 8, valid with its
   complement in byte 10) as the reference. The reset the boot ends comes later,
   so the poll sees it as the first game start, see ram_mirror_poll(). */
static unsigned char ram_mirror_probe(void) {
  unsigned char hdr_tx[RAM_MIRROR_HEAD] = { 0 }, v = 0xE1;
  for(int i = 0; i < 50 && (!ram_mirror_data || ram_mirror_resets < 0); i++) {
    if(i) vTaskDelay(pdMS_TO_TICKS(10));
    v = ram_mirror_header(hdr_tx);
    if(v == 0) {
      if(ram_mirror_buf[10] == (unsigned char)~ram_mirror_buf[8]) ram_mirror_resets = ram_mirror_buf[8];
      mcu_hw_spi_end();                    /* the header is all the probe wants */
    }
  }
  if(!ram_mirror_data)
    debugf("RAM mirror: no valid layout %u header in 50 tries, the board is unknown", RAM_MIRROR_LAYOUT);
  return ram_mirror_data ? 0 : v;
}

/* game20k: another core came up, its header is read anew as at the start */
static void ram_mirror_forget(void) {
  ram_mirror_data    = 0;
  ram_mirror_board   = 0;
  ram_mirror_tag     = 0;
  ram_mirror_resets  = -1;
  ram_mirror_frame   = -1;
  ram_mirror_verdict = 0xA5;
  ra_patch_board(0);
}

/* Read the header first. Byte 7 set means a harvest was running when the transfer
   started: the shadow is half old, half new, so skip it and retry on the next poll.
   That is normal, the verdict stays. From the verdict byte on, the FPGA starts no
   new harvest until chip select rises, so a clear byte 7 holds to the end. A harvest
   starting in exactly that clock tears the snapshot, the checksum (0xE6) catches it.
   Every offset in the body comes from the length the header announced. */
static void ram_mirror_poll(void) {
  unsigned char hdr_tx[RAM_MIRROR_HEAD] = { 0 };
  banner_follow_game();
  ra_events_header(hdr_tx);

  // the banner, one character per poll in bytes 6 and 7
  banner_login();
  banner_lboard();
  if(switch_step()) return;   /* another core came up, the next poll reads it */
  banner_follow_game();       /* a failed core switch settles in switch_step() */
  banner_step(hdr_tx);

  /* Verdict: 0xA5 ok, otherwise the failed check. 0xE1 magic, 0xE2 layout, 0xE3
     board and length, 0xE4 frame number, 0xE6 checksum, 0xE7 log overflow, 0xE8
     and 0xE9 see below. */
  if(ram_mirror_header(hdr_tx)) return;      /* SPI ended, verdict set */
  if(ram_mirror_buf[7] != 0x00) {            /* harvest running, retry next poll */
    mcu_hw_spi_end(); return;
  }
  /* frame number counts harvests: same as the last good snapshot means nothing new */
  int frame = ram_mirror_buf[5] | (ram_mirror_buf[6] << 8);
  if(frame == ram_mirror_frame) {
    mcu_hw_spi_end(); return;
  }

  const unsigned d = ram_mirror_data;        /* this core's game RAM, checked by the header */
  mcu_hw_spi_rx_block(ram_mirror_buf + RAM_MIRROR_HEAD, d + RAM_MIRROR_LOG + RAM_MIRROR_TAIL);
  mcu_hw_spi_end();

  /* Same checksum the FPGA forms when it fills the FIFO, over game RAM and oracle
     log. Rotate before XOR so that the byte order counts. */
  unsigned short sum = 0;
  for(unsigned int i = RAM_MIRROR_HEAD; i < RAM_MIRROR_HEAD + d + RAM_MIRROR_LOG; i++)
    sum = (unsigned short)(((sum << 1) | (sum >> 15)) ^ ram_mirror_buf[i]);

  const unsigned char *ftr  = ram_mirror_buf + RAM_MIRROR_HEAD + d + RAM_MIRROR_LOG;
  unsigned short       want = (unsigned short)(ftr[4] | (ftr[5] << 8));

  /* Checksum before the underrun bit: the bit only says the FIFO ran empty, the
     checksum says whether data arrived wrong. */
  if(ram_mirror_buf[5] != ftr[0] ||
     ram_mirror_buf[6] != ftr[1])                 { ram_mirror_verdict = 0xE4; return; }
  if(sum != want)                                 { ram_mirror_verdict = 0xE6; return; }
  if(ftr[3] != 0x00)                              { ram_mirror_verdict = 0xE7; return; }

  /* Underrun with a good checksum did no harm: keep the snapshot, report 0xE8. */
  unsigned char note = (ftr[2] != 0x00) ? 0xE8 : 0xA5;

  /* Oracle. The log holds every game write during the harvest, and the snapshot must
     carry the last value written to each of those addresses. Walk the log backwards
     and check only the first hit per address. Other addresses cannot differ. */
  unsigned int n = (unsigned int)ftr[6] | (((unsigned int)ftr[7] & 3u) << 8);
  if(n > RAM_MIRROR_LOG / 3) n = RAM_MIRROR_LOG / 3;

  for(unsigned int i = 0; i < (d + 7) / 8; i++) ram_mirror_seen[i] = 0;   /* one bit per byte of this game's RAM */

  unsigned short bad = 0;
  for(int i = (int)n - 1; i >= 0; i--) {
    const unsigned char *e = ram_mirror_buf + RAM_MIRROR_HEAD + d + 3 * i;
    unsigned short flat = (unsigned short)(e[0] | (e[1] << 8));
    if(flat >= d) continue;                                /* cannot happen, guards the index */
    if(ram_mirror_seen[flat >> 3] & (1u << (flat & 7))) continue;
    ram_mirror_seen[flat >> 3] |= (unsigned char)(1u << (flat & 7));
    if(ram_mirror_buf[RAM_MIRROR_HEAD + flat] != e[2]) bad++;
  }
  if(bad) { ram_mirror_verdict = 0xE9; return; }     /* snapshot does not match the log */

  ra_events_prepare();
  /* The core counts the ends of its resets, S1 on the Nano as well as the menu's
     reset (byte 8, taken when the harvest ended). A new count is a new game: rcheevos
     starts over before it sees this snapshot, and only here, the Companion's own
     reset ('R') counts the same way. The probe took the reference, without one the
     first snapshot is the start. Byte 9, the core's diagnostic parameters, decides
     whether hardcore is possible at all, before the first frame is evaluated. */
  /* Bytes 10 and 11 carry the complements of 8 and 9: the checksum covers only the
     body, and a wrong bit here would reset rcheevos or block hardcore for nothing.
     A pair that does not match is left out for this snapshot. */
  if(ram_mirror_buf[10] == (unsigned char)~ram_mirror_buf[8] &&
     ram_mirror_buf[11] == (unsigned char)~ram_mirror_buf[9]) {
    int resets = ram_mirror_buf[8];
    if(resets != ram_mirror_resets) {
      ra_patch_core_reset();
      ram_mirror_resets = resets;
    }
    ra_task_core_flags(ram_mirror_buf[9]);
  }
  ra_events_frame(ram_mirror_buf + RAM_MIRROR_HEAD, ram_mirror_data);

  ram_mirror_frame   = frame;
  ram_mirror_verdict = note;
}


#include "../ftpd.h"
#include "../telnetd.h"
#include "../xml.h"
#include "../at_wifi.h"

/*-----------------------------------------------------------*/
/*---            main FPGA communication task            ----*/
/*-----------------------------------------------------------*/

/* game20k: a switch to a core of another board hops the ring here, right after the
   first look at the header and before anything of the core is set up (menu,
   settings, ROM): one start of the Pico for the whole way instead of one per hop.
   The way is that of switch_rom(), back to the board of the pick after a whole
   round without the game's board. Each hop pulls RECONFIG_N (Z), waits for the next
   core and its card and reads the new board. The marker in the scratch registers
   follows each hop, so a core that does not come up ends in a restart that goes on
   from there as before. The marker in RAM ends at the board reached, switch_rom()
   then loads the ROM or says why not. probe takes the verdict of the header of
   the last core loaded. True when another core came up.
   A switch while a game runs comes here as well, see switch_step(): the marker
   it wrote is the way, the first hop goes from the running core. */
static bool ring_hops(unsigned char *probe) {
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
  } else
    return false;
  const char *title = g ? g->title : "the game picked";
  // a core the way only passes is reloaded before its card is ready, the next core
  // starts the card anew: the card lock stays held from one hop to the next, and only
  // the core the way ends at is waited for, see below the loop
  bool locked = false;
  for(;;) {
    if(!HOPS_ORIGIN(hops) && !HOPS_COUNT(hops)) hops |= (uint32_t)ram_mirror_board << 16;
    bool back = hops & HOPS_BACK;
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
      hops = HOPS_BACK | (hops & 0xff0000u);
      target = HOPS_ORIGIN(hops);
    } else
      hops++;
    if(g) switch_mark(g, hops);
    else  switch_mark_file(id, board, hops);
    for(;;) {                    /* S1 and S2 read under the lock, right before Z */
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
    debugf("Core switch for %s, board %u, on from board %u within this start", title, target, ram_mirror_board);
    if(!core_switch()) {         /* the bitstream does not know Z: switch_rom() says so */
      sdc_unlock();
      watchdog_hw->scratch[0] = 0;
      return hopped;
    }
    if(!sys_wait4fpga()) {
      debugf("Core switch: no core after the hop, restart");
      mcu_hw_reset();            /* the marker carries the way on */
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
  watchdog_hw->scratch[0] = 0;
  return hopped;
}

/* game20k: the menu of the core that runs, parsed into cfg, at the start and after
   a core switch (core_up()). Returns true for a game20k core whose menu this
   firmware lacks: the menu says so once it runs. */
static bool core_config(unsigned char probe) {
  bool mismatch = false;

  // try to load a config .xml from sd card. sys_get_config_name() always
  // returns /sd/config.xml here: the core id byte stays 0 (game20k). Under the
  // card lock: after a core switch FTP may be at the card meanwhile
  FIL fil;
  sdc_lock();
  if(f_open(&fil, sys_get_config_name(), FA_OPEN_EXISTING | FA_READ) == FR_OK) {
    config_init();

    UINT br; char c;
    debugf("Loading XML config from file");
    // a menu of one's own can set the DIP switches without a reset, and it is
    // what decides the mode: no hardcore with it (the core also takes DIP
    // switches only while in reset)
    ra_task_hardcore_block(RA_HC_BLOCK_XML, true);

    // read byte by byte. Slow but that doesn't hurt ...
    FRESULT r = f_read(&fil, &c, 1, &br);
    while(r == FR_OK && br) {
      xml_parse(c);
      r = f_read(&fil, &c, 1, &br);
    }
    f_close(&fil);
    sdc_unlock();

    config_dump();
  } else {
    sdc_unlock();
    // game20k: no XML on SD card. The firmware's menu for the core's board when
    // the interface tags agree. Else a core without a game20k header may bring
    // its own menu (READ_CFG), and the last resort is the basic menu: there is
    // always an OSD. A game20k core of another release gets the basic menu with
    // a message, and without hardcore when it has the board but another tag:
    // its DIP switches may mean something else than this firmware's menu says.
    const menus_entry_t *m;
    menus_why_t why = menus_pick(ram_mirror_board, ram_mirror_tag, NULL, &m);
    const char *c = NULL;
    if(m) {
      debugf("Loading the built-in XML config of board %u", ram_mirror_board);
      c = m->xml;
    } else if(why == MENUS_NO_HEADER && (c = sys_get_config()) != NULL)
      debugf("Loading XML config from core");
    else {
      debugf("No menu for this core (board %u, tag %04x, why %d), loading the basic XML config",
             ram_mirror_board, ram_mirror_tag, why);
      c = menus_basic_xml;
    }
    mismatch = why != MENUS_FOUND && probe != 0xE1;
    ra_task_hardcore_block(RA_HC_BLOCK_MENU, why == MENUS_MISMATCH);   // the core before may have set it
    config_init();
    while(*c) xml_parse(*c++);
    config_dump();
  }
  return mismatch;
}

/* game20k: the images of the menu's init action, the game picked before, the
   settle and the ready action, at the start and after a core switch. */
static void core_start(void) {
  // open disk images, either defaults set in sdc_init or
  // user configure ones from the ini file. This will also
  // start rom image transfers if specified in the ini file
  switch_rom();            /* a game picked before a core switch goes first */
  sdc_mount_defaults();

  // game20k: no ROM in the core, its file is missing, old or rejected: the
  // Games page opens instead of a dark screen with a closed menu
  if(!sdc_get_image_name(MAX_DRIVES + RA_PATCH_ROM_IMAGE)) menu_notify(MENU_EVENT_GAMES);

  // game20k: no image0 in the ini, or the core rejected it (the transfer is
  // closed then): the game is decided now, before the ready action, from the
  // board alone. With a stream running, its last chunk settles.
  if(!sdc_image_upload_in_progress()) ra_patch_settle();

  // finally run the ready action. This will usually get the core out of reset
  // On setups not using core configs, just release FPGA from reset
  // But this should actually never be the case nowadays.
  // TODO: An image upload may still be in progress ...
  if(!sdc_image_upload_in_progress()) {
    if(!cfg) sys_set_val('R', 0);
    else     sys_run_action_by_name("ready");
  } else
    debugf("Image upload in progress, delaying ready action");
}

/* game20k: a new core is up, set it up as the start sets one up: the old core's
   images and game go, its menu is freed, the new one is parsed and built with the
   variables and the ini of the new core, and its game streams. WiFi, the login,
   the clock, FTP and telnet stay. The menu task is parked, probe is the verdict on
   the new core's header, t0 when the switch began. */
static void core_setup(unsigned char probe, TickType_t t0) {
  sdc_lock();
  sdc_forget_images();
  sdc_unlock();
  ra_patch_new_core();
  banner_restart();
  // the old menu goes, the log says what the new one takes
  size_t before = xPortGetFreeHeapSize();
  config_free();
  size_t freed = xPortGetFreeHeapSize();
  bool mismatch = core_config(probe);
  debugf("Core switch: the old config freed %u bytes, the new one takes %u, FreeRTOS heap free %u",
         (unsigned)(freed - before), (unsigned)(freed - xPortGetFreeHeapSize()),
         (unsigned)xPortGetFreeHeapSize());
  sys_handle_interrupts(sys_irq_ctrl(0xff), true);   // what the cores of the way left
  hid_handle_event();                                // the new core's DB9 interrupts, as at the start
  osd_init();
  menu_rebuild();
  if(mismatch) menu_notify(MENU_EVENT_MISMATCH);
  core_start();
  debugf("Core switch: board %u up after %lu ms, no restart", ram_mirror_board,
         (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
}

/* game20k: the core switch without a restart of the Pico, from switch_step() with
   the menu task parked and the way in the scratch registers. ring_hops() loads the
   cores of the way, core_setup() the last one. False when the running bitstream
   does not know Z: nothing changed, the card lock is free again. */
static bool core_up(void) {
  TickType_t t0 = xTaskGetTickCount();
  unsigned char probe;
  for(int i = 0; i < 4; i++) marker[i] = watchdog_hw->scratch[i];
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
  ra_patch_switch_cancel();
  marker[0] = 0;                 /* no game picked: switch_rom() leaves the ini's ROM */
  ram_mirror_forget();
  core_setup(ram_mirror_probe(), t0);
  return true;
}

TaskHandle_t com_task_handle = NULL;

static void com_task(__attribute__((unused)) void *p ) {
  debugf("Starting main communication task");
  marker_take();             /* game20k: the marker of a restart, first of all */
  
  // startup FPGA, this will also put the core into reset
  if(sys_wait4fpga()) {
    // FPGA is ready and can be talked to

    // initialitze SD card
    sdc_init();

    // try to load the global config
    inifile_config_read();

    // game20k: the board id and the interface tag of the core from the RAM mirror
    // header, before the menu: they pick the menu built into this firmware. The
    // board also decides with the ROM which game this is. After the menu comes the
    // handover of the achievement set. The set itself is read in ra_patch_settle()
    // once board and ROM are known, so a set with a valid tag lets the game start in
    // hardcore. rcheevos takes it with the first snapshot.
    unsigned char probe = ram_mirror_probe();
    start_game();             /* game20k: at power-on the start game of Save settings */
    ring_hops(&probe);        /* a game of another board: its core first, see ring_hops() */
    bool mismatch = core_config(probe);   // game20k: the menu, see core_config()

    // process any pending interrupt. Filter out irq 1 which is the
    // FPGA cold boot event which we ignore since we just booted outselves
    sys_handle_interrupts(sys_irq_ctrl(0xff), true);
    
    // by default, DB9 interrupts are disabled. Reading
    // the DB9 state enables them. This is what hid_handle_event
    // does.
    hid_handle_event();

    // initialize on-screen-display and menu system
    osd_init();    
    menu_init();
    if(mismatch) menu_notify(MENU_EVENT_MISMATCH);

    if(!ra_patch_init()) debugf("RA: set handover could not be set up, no achievements");

    core_start();             /* game20k: the images, the game, the ready action */

    ftpd_init();

    // finally prepare for wifi communication
    at_wifi_init();

    debugf("Entering main loop");

    // game20k: the RA task starts from the loop once the game is settled, and
    // before that pass's poll, so the first snapshot and any unlock find the
    // task and its handover queue. A stream ejected in the OSD settles on the
    // next pass with "no ROM"; a file picked in the OSD holds the settle back
    // (rom_pending) until its stream ends; a stream that stalls settles after
    // 15 s at the latest, with what is known by then.
    bool ra_started = false;
    TickType_t loop_start = xTaskGetTickCount();
    for(;;) {
      mcu_hw_irq_ack();  // (re-)enable interrupt
      /* wake at least every 20 ms to poll the RAM mirror, not only on an interrupt */
      if(ulTaskNotifyTake( pdTRUE, pdMS_TO_TICKS(20) ))
        sys_handle_interrupts(sys_irq_ctrl(0xff), false);
      if(!ra_patch_settled() &&
         ((!sdc_image_upload_in_progress() && !ra_patch_rom_pending()) ||
          (xTaskGetTickCount() - loop_start) >= pdMS_TO_TICKS(15000)))
        ra_patch_settle();
      if(!ra_started && ra_patch_settled()) {
        // RetroAchievements talks to the server in a task of its own,
        // so this loop never waits for it
        ra_task_start();
        ra_started = true;
      }
      ram_mirror_poll();
    }
  }

  /* This will only be reached if the FPGA is not ready */
  /* So loop foreever while e.g. USB is still being handled */
  /* e.g. for debugging */
  for(;;) {
    // frequently check for an FPGA to show up and reboot to
    // startup normally if one is detected
    if(sys_status_is_valid()) {
      debugf("FPGA detected!");
      // This may be due to a USB download. So give USB
      // some time to finish. The same happens in sysconfig.c
      vTaskDelay(pdMS_TO_TICKS(2000));
      mcu_hw_reset();
    }
      
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  // the device key for the tags on the card, before anything else runs: making it
  // the first time erases a flash sector with interrupts off, see ra_mac.c
  ra_mac_init();

  mcu_hw_init();
  telnetd_init();
  
  // run FPGA com thread. game20k: 12 KB of stack, about 2.5 times what it used on the Pico
  // with rcheevos running a set
  xTaskCreate( com_task, "FPGA Com", 3072, NULL, CONFIG_MAX_PRIORITY-1, &com_task_handle );

  mcu_hw_main_loop();

#ifndef ESP_PLATFORM
  return 0;
#endif
}
/*-----------------------------------------------------------*/

