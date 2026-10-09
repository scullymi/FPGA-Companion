/*
  main.c - MiSTeryNano FPGA Companion Pi Pico variant

*/

#include "../mcu_hw.h"
uint32_t getFreeHeap(void);   /* mcu_hw.c: the SDK heap left for mbedTLS and rcheevos */

#include "../config.h"
#include "../sysctrl.h"
#include "../sdc.h"
#include "../osd.h"
#include "../menu.h"
#include "../inifile.h"
#include "../debug.h"
#include "../spi.h"     /* SPI_TARGET_RAM */
#include "rc_runtime.h"
#include "../ra_patch.h"
#include "../ra_queue.h"
#include "../ra_state.h"
#include "../ra_task.h"
#include "../ra_mac.h"
#include <string.h>
#include "pico/time.h"
#include "hardware/watchdog.h"   /* the game to start after a restart, see restart_step() */
#include "../ftpd.h"             /* a restart waits for the uploads that run */
#include "../games.h"            /* the Games page's pick and the start game */
#include "../menus.h"            /* the menus of game20k's cores */

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

/* rcheevos evaluates the achievement conditions over each good snapshot. The set
   is defined on the flat layout the core sends, per game (Galaga: bgram 2048, then
   wram1..3 of 1024 each), so an address is the offset in the snapshot. */
static rc_runtime_t   ra_rt;
static bool           ra_ready;
static unsigned char  ra_triggered;         /* achievements triggered, saturating */
static unsigned char  ra_last;              /* last one, 1-based position in the set */
static unsigned short ra_us;                /* evaluation time of the last snapshot */
static unsigned       ra_oob;               /* reads outside the mirror */
static unsigned       ra_rp_frames;         /* frames since the rich presence text was read */
#define RA_RP_EVERY   60                    /* frames between two readings, about a second */

static uint32_t ra_peek(uint32_t address, uint32_t num_bytes, void *ud) {
  (void)ud;
  /* outside the running core's game RAM there is nothing, say so once instead of
     reading air, or stale bytes of a bigger game in the buffer. Compared without a
     sum, so that an address near 2^32 cannot wrap into range. */
  if(address >= ram_mirror_data || num_bytes > ram_mirror_data - address) {
    if(!ra_oob++) debugf("RA: condition reads 0x%lx, outside the mirror", (unsigned long)address);
    return 0;
  }
  const unsigned char *p = ram_mirror_buf + RAM_MIRROR_HEAD + address;
  switch(num_bytes) {
  case 1: return p[0];
  case 2: return p[0] | (p[1] << 8);
  case 4: return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  default: return 0;
  }
}

/* The banner at the bottom of the picture: 24 characters the FPGA shows for a
   while. It carries the title of an unlock, or a message that starts with who
   speaks, "RA: " for the achievements and "SYS: " for the machine itself. One
   character goes over with each mirror poll in header bytes 6 and 7. The show
   flag follows once the whole text is in the FPGA, so it appears at once. Banners
   that arrive while one is shown wait their turn, a few of them. */
#define BANNER_LEN   24
#define BANNER_MS    8000
#define BANNER_QUEUE 4
#define BANNER_PROGRESS_MS 2500   /* a progress banner is short, the next one may follow soon */
typedef struct {
  char text[BANNER_LEN];
  bool gold;        /* text gold for hardcore, white for softcore, as on the RA site */
  bool new;         /* mark green: new, goes to the server. Grey: the account has it, or it is queued */
  unsigned ms;      /* how long it shows */
} banner_t;
static banner_t   banner;                  /* the one being sent or shown */
static banner_t   banner_q[BANNER_QUEUE];  /* the ones waiting, oldest first */
static unsigned   banner_n;                /* how many wait */
static unsigned   banner_pos;              /* next character to send */
static bool       banner_pending;          /* text on its way, show follows after the pass */
static bool       banner_live;             /* show flag on */
static TickType_t banner_start;            /* when the show flag went on */
static bool       banner_mode_due;         /* a game started or the mode changed: say which mode counts */
static banner_t   banner_progress;         /* the latest progress, shown only when no other banner waits */
static bool       banner_progress_due;

/* Fits a text into a banner: the FPGA font has A-Z, 0-9, space and ! - . : so
   lower case is raised and anything else becomes a space, a longer text ends at
   a word where it can, and the rest of the line is spaces. */
static void banner_fill(banner_t *b, const char *text, bool gold, bool new, unsigned ms) {
  unsigned i = 0;
  for(; text[i] && i < BANNER_LEN; i++) {
    char c = text[i];
    if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(" !-.:", c))) c = ' ';
    b->text[i] = c;
  }
  // a longer text ends at a word, if one ends in the last third of the line,
  // rather than in the middle of one. Titles can have up to 64 characters.
  if(text[i] && text[i] != ' ') {
    unsigned cut = i;
    while(cut > BANNER_LEN * 2 / 3 && b->text[cut - 1] != ' ') cut--;
    if(cut > BANNER_LEN * 2 / 3) i = cut - 1;
  }
  for(; i < BANNER_LEN; i++) b->text[i] = ' ';
  b->gold = gold;
  b->new  = new;
  b->ms   = ms;
}

static void banner_show(const char *text, bool gold, bool new) {
  banner_t b;
  if(banner_n >= BANNER_QUEUE) {
    debugf("RA: banner '%s' dropped, %u already wait", text, banner_n);
    return;
  }
  banner_fill(&b, text, gold, new, BANNER_MS);
  banner_q[banner_n++] = b;
  debugf("RA: banner '%.*s' for %u s (%s text, %s mark)%s", BANNER_LEN, b.text, BANNER_MS / 1000,
         gold ? "gold" : "white", new ? "green" : "grey", banner_n > 1 ? ", waits" : "");
}

/* The progress of an achievement as a short banner, "12/50 TITLE". Only the
   latest counts: it waits until no other banner shows or waits, and a newer one
   replaces it. RetroAchievements asks that progress shows during play. */
static void banner_show_progress(const char *progress, const char *title, bool gold) {
  char text[BANNER_LEN + 1];
  size_t n = strlen(progress);
  // the FPGA font has no %, a percentage reads "37 PCT"
  if(n && progress[n - 1] == '%')
    snprintf(text, sizeof(text), "%.*s PCT %s", (int)(n - 1), progress, title);
  else
    snprintf(text, sizeof(text), "%s %s", progress, title);
  banner_fill(&banner_progress, text, gold, false, BANNER_PROGRESS_MS);
  banner_progress_due = true;
}

/* One poll's worth of banner work: ends a banner after its time, starts the
   next waiting one, then a waiting progress, and fills header bytes 6 and 7.
   Bit 7 show, bit 6 gold text, bit 5 green mark, bits 4..0 the position, byte 7
   the character. */
static void banner_step(unsigned char *hdr) {
  if(banner_live && (xTaskGetTickCount() - banner_start) >= pdMS_TO_TICKS(banner.ms))
    banner_live = false;
  if(!banner_live && !banner_pending && banner_n) {
    banner = banner_q[0];
    memmove(banner_q, banner_q + 1, (--banner_n) * sizeof(banner_q[0]));
    banner_pos     = 0;
    banner_pending = true;
  } else if(!banner_live && !banner_pending && banner_progress_due) {
    banner = banner_progress;
    banner_progress_due = false;
    banner_pos     = 0;
    banner_pending = true;
  }
  hdr[6] = (unsigned char)((banner_live ? 0x80 : 0) | (banner.gold ? 0x40 : 0) |
                           (banner.new ? 0x20 : 0) | (banner_pos & 0x1f));
  hdr[7] = (unsigned char)banner.text[banner_pos];
  if(++banner_pos >= BANNER_LEN) {
    banner_pos = 0;
    if(banner_pending) {            // the whole text is over, now it may show
      banner_pending = false;
      banner_live    = true;
      banner_start   = xTaskGetTickCount();
    }
  }
}

/* A message when the RA task's state changes: the account is in, or the
   player should know why nothing counts yet. Then the mode, at each game start
   and when it changes, as RetroAchievements asks: the player sees what counts. */
static void banner_login(void) {
  static ra_task_state_t shown = RA_TASK_STARTING;
  static bool offline_told;                /* the offline message once, until the login */
  static int  mode_shown = -1;
  ra_task_state_t now = ra_task_state();
  int mode = ra_task_hardcore();
  if(now != shown) {
    shown = now;
    // messages are white, the mark is green when all is well and grey when not
    if(now == RA_TASK_LOGGED_IN) {
      char text[BANNER_LEN + 1];
      snprintf(text, sizeof(text), "RA: %s", inifile_config_get_str("ra", "user"));
      banner_show(text, false, true);
    } else if(now == RA_TASK_REJECTED)
      banner_show("RA: LOGIN REJECTED", false, false);
    else if(now == RA_TASK_NO_TIME)
      banner_show("RA: NO TIME SERVER", false, false);
    else if(now == RA_TASK_NO_GAME)
      banner_show("RA: NO GAME", false, false);
    else if(now == RA_TASK_RETRYING && !offline_told) {
      // no server: achievements still count, their unlocks wait on the card
      offline_told = true;
      banner_show("RA: OFFLINE UNLOCKS KEPT", false, false);
    }
  }
  if(mode != mode_shown) {
    mode_shown = mode;
    banner_mode_due = true;
  }
  // once the task knows whether there is an account: without one nothing counts.
  // Softcore although the menu asks for hardcore names the reason. The same text
  // again within a few seconds is left out: a reset from the menu also changes the
  // core's reset count, both ask for this banner. While a ROM streams to the core
  // the banner waits: the stream blocks hardcore until its digest is checked, a
  // second later, and the settle after it names the mode that counts.
  if(banner_mode_due && now != RA_TASK_STARTING && !ra_patch_rom_pending()) {
    static const char *last_text;
    static TickType_t  last_tick;
    unsigned why = ra_task_hardcore_blocked();
    const char *text = mode ? "RA: HARDCORE" :
                       !ra_task_hardcore_wanted()   ? "RA: SOFTCORE" :
                       (why & RA_HC_BLOCK_CORE)     ? "RA: SOFTCORE TEST CORE" :
                       (why & RA_HC_BLOCK_XML)      ? "RA: SOFTCORE CONFIG.XML" :
                       (why & RA_HC_BLOCK_MENU)     ? "RA: SOFTCORE MISMATCH" :
                       (why & RA_HC_BLOCK_KEY)      ? "RA: SOFTCORE KEY ERROR" :
                       (why & RA_HC_BLOCK_SIZE)     ? "RA: SOFTCORE SET TOO BIG" :
                       // a wrong game also explains an unknown ROM, so it comes first
                       (why & RA_HC_BLOCK_GAME)     ? "RA: SOFTCORE WRONG GAME" :
                       (why & RA_HC_BLOCK_ROM)      ? "RA: SOFTCORE ROM UNKNOWN" :
                       // without a login the set never comes, the login banner says why
                       (why & RA_HC_BLOCK_SET) && now != RA_TASK_REJECTED ? "RA: SOFTCORE TILL ONLINE" :
                       "RA: SOFTCORE";
    banner_mode_due = false;
    // without an account or a game nothing counts, their own banner says so
    if(now != RA_TASK_NO_ACCOUNT && now != RA_TASK_NO_GAME &&
       !(text == last_text && (xTaskGetTickCount() - last_tick) < pdMS_TO_TICKS(5000))) {
      char warn[RA_PATCH_TITLE_MAX], line[4 + RA_PATCH_TITLE_MAX];
      banner_show(text, mode, true);
      last_text = text;
      last_tick = xTaskGetTickCount();
      // in hardcore the server's warning follows, as rc_client shows it. While
      // RetroAchievements has not approved this client, it keeps the unlocks as
      // casual, and the player sees that next to the mode the device plays in.
      if(mode && ra_patch_warning(warn, sizeof(warn))) {
        snprintf(line, sizeof(line), "RA: %s", warn);
        banner_show(line, false, false);
      }
    }
  }
}

/* Challenges that are on, one bit per position in the set's table. An
   achievement is primed when all its conditions but the trigger hold, e.g. a
   stage without losing a ship, and RetroAchievements asks that this shows during
   play: header byte 5 bit 0 lights a marker next to the picture. */
static uint32_t ra_primed[(RA_PATCH_MAX + 31) / 32];

static void ra_primed_set(unsigned id, bool on) {
  unsigned i = ra_patch_index(id);
  if(!i || i > RA_PATCH_MAX) return;
  i--;
  if(on) ra_primed[i / 32] |=  (1u << (i % 32));
  else   ra_primed[i / 32] &= ~(1u << (i % 32));
}

static bool ra_primed_any(void) {
  for(unsigned w = 0; w < sizeof(ra_primed) / sizeof(ra_primed[0]); w++)
    if(ra_primed[w]) return true;
  return false;
}

/* Unlocked in the mode that counts, those show neither challenge nor progress. */
static bool ra_done(unsigned id) {
  return ra_state_known(id) || (!ra_task_hardcore() && ra_state_softcore_only(id));
}

/* Leaderboards: an attempt starts, ends without a result, or is finished with a
   value. Only hardcore counts, the server keeps an entry as a hardcore one and
   only from a client it has approved: in softcore a result only goes to the
   log. A banner when an attempt starts and one with the result, no live
   tracker, the value is the score the game shows anyway. RetroAchievements
   allows leaderboard popups to be off, not the submission. */
static void ra_lboard_event(const rc_runtime_event_t *ev) {
  ra_patch_lboard_t lb;
  char text[64], value[24];   // banner_show() cuts to the banner, at a word where it can
  bool hardcore = ra_task_hardcore();
  if(!ra_patch_lboard(ev->id, &lb)) return;
  switch(ev->type) {
  case RC_RUNTIME_EVENT_LBOARD_STARTED:
    debugf("RA: leaderboard %u started: %s", (unsigned)ev->id, lb.title);
    if(hardcore) {
      snprintf(text, sizeof(text), "RA: LB %s", lb.title);
      banner_show(text, true, false);
    }
    break;
  case RC_RUNTIME_EVENT_LBOARD_CANCELED:
    debugf("RA: leaderboard %u attempt ended without a result", (unsigned)ev->id);
    break;
  case RC_RUNTIME_EVENT_LBOARD_TRIGGERED:
    rc_runtime_format_lboard_value(value, sizeof(value), ev->value, lb.format);
    if(!hardcore) {
      debugf("RA: leaderboard %u: %s, not submitted in softcore", (unsigned)ev->id, value);
      break;
    }
    debugf("RA: leaderboard %u: %s, goes to the server", (unsigned)ev->id, value);
    snprintf(text, sizeof(text), "RA: LB RESULT %s", value);
    banner_show(text, true, true);
    ra_task_lboard(ev->id, ev->value);
    break;
  default:
    break;   // UPDATED comes with every change of the value, DISABLED needs address checks this firmware does not make
  }
}

/* A ROM picked after the start can be a known ROM of another game of this board,
   e.g. puckman.rom in the Pac-Man core: its own set, session and card folder come
   only with a start (ra_patch_restart_to()). The Pico restarts into it: the menu
   says so for a few seconds. Unlocks still in the handover reach the card,
   leaderboard results the server, as far as that takes at most 10 s, a ROM picked
   meanwhile ends its stream (each within 10 s of its start), and a running FTP
   upload then stops after its chunk.
   The game's id goes into the watchdog's scratch registers 0 to 3, which the SDK
   leaves to the program and which a reboot keeps, and the Pico restarts.
   restart_take() moves them out at the very start, restart_rom() loads that game's
   ROM instead of image0 of the ini, once: power off brings back the saved game,
   "Save settings" keeps the new one, as before. The newest pick wins until the
   marker is written, see ra_patch_restart_to().
   A game of another board, e.g. galaga.rom picked in the Pac-Man core
   (ra_patch_pick_other_board()), needs its core first: the same steps, but
   before the restart the core pulls RECONFIG_N (Z = 0xA5, see game20k_top.sv) and
   the FPGA loads the next core of the flash ring (fpga/common/slots.txt). The
   Pico restarts while it loads, waits for it at the start as after power on, and
   restart_rom() finds the board it wanted or switches on to the next core. Power
   on loads the first core of the ring again.
   A game picked on the Games page goes the same way, as a file: magic RESTART_FILE,
   the CRC-32 of its file name instead of the id, and its board with the hops, so a
   game without a row in the table (no achievement set) starts as well. */
#define RESTART_MAGIC   0x67323072u          /* "g20r" */
#define RESTART_FILE    0x67323066u          /* "g20f" */
#define RESTART_SHOW_MS 3000                 /* the menu's message first */
#define RESTART_WAIT_MS 10000                /* then at most this for unlocks and results */
#define RESTART_STOP_MS 12000                /* then this for an upload to stop after its chunk, its data socket times out after 10 s */
#define SWITCH_HOPS     7                    /* the 8 MB flash holds at most 8 cores of 1 MB */

/* The game for the next start, by its row in ra_games.c: a game and its regional sets
   share their RetroAchievements id (gng and makaimurg), the row tells them apart.
   hops counts the core switches on the way to its board, the check covers it too. */
static void restart_mark(const ra_game_t *g, uint32_t hops) {
  uint32_t row = ra_games_row(g);
  watchdog_hw->scratch[0] = RESTART_MAGIC;
  watchdog_hw->scratch[1] = row;
  watchdog_hw->scratch[3] = hops;
  watchdog_hw->scratch[2] = ~(RESTART_MAGIC ^ row ^ hops);
}

/* A file for the next start, by the CRC-32 of its name in the card's root, with
   the board its core has in scratch register 3 above the hops. */
static void restart_mark_file(uint32_t crc, unsigned char board, uint32_t hops) {
  uint32_t b = hops | (uint32_t)board << 8;
  watchdog_hw->scratch[0] = RESTART_FILE;
  watchdog_hw->scratch[1] = crc;
  watchdog_hw->scratch[3] = b;
  watchdog_hw->scratch[2] = ~(RESTART_FILE ^ crc ^ b);
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

static void restart_step(void) {
  static TickType_t start;
  static uintptr_t  shown;           /* the target the message and the wait are for, 0 for none */
  // a pick on the Games page goes first, else the game a ROM picked under "ROM set" asks for
  games_pick_t pk;
  bool file = games_picked(&pk);
  const ra_game_t *g = file ? NULL : ra_patch_restart_to();
  if(!file && !g) {                  /* none, or the ROM picked last took it back */
    if(shown) {
      ftpd_hold_uploads(false);
      ftpd_stop_uploads(false);
    }
    start = 0;
    shown = 0;
    return;
  }
  // a pick is told apart by its count, odd so it never equals an entry's address
  uintptr_t key = file ? ((uintptr_t)pk.seq << 1) | 1 : (uintptr_t)g;
  unsigned char board = file ? pk.board : g->board;
  const char *title = file ? pk.title : g->title;
  TickType_t now = xTaskGetTickCount();
  if(!start || key != shown) {       /* a new target: its message, and the wait starts over */
    start = now ? now : 1;
    shown = key;
    ftpd_hold_uploads(true);         /* no new upload, the restart would cut it */
    menu_notify(MENU_EVENT_RA_RESTART);
    return;
  }
  TickType_t since = now - start;
  if(since < pdMS_TO_TICKS(RESTART_SHOW_MS)) return;
  // unlocks and leaderboard results live in RAM until the card or the server
  // has them, an upload runs until its file is closed, a ROM picked last streams
  // until its settle decides the target: wait for them, but not for ever
  bool late = since >= pdMS_TO_TICKS(RESTART_SHOW_MS + RESTART_WAIT_MS);
  bool cut  = since >= pdMS_TO_TICKS(RESTART_SHOW_MS + RESTART_WAIT_MS + RESTART_STOP_MS);
  if((ra_queue_in_transit() || ra_task_lboard_pending()) && !late)
    return;
  if(ra_patch_rom_streaming(RESTART_WAIT_MS))   /* its own limit: also a pick made late wins */
    return;
  // an upload still running then stops after its chunk with 426, its file closed
  // whole: wait for that too, only a stalled one is cut
  if(late) ftpd_stop_uploads(true);
  if(ftpd_uploads() && !cut)
    return;
  // the last checks under the card lock, where they hold until the Pico goes:
  // every card operation ends whole, an upload opens its file and a ROM starts to
  // stream only under the lock, S1 and S2 are read right before Z and not before a
  // wait for the lock. The target is fixed last, a pick can change it until then
  sdc_lock();
  bool held = board != ram_mirror_board && buttons_held();
  bool busy = ra_patch_rom_streaming(RESTART_WAIT_MS) || (!cut && ftpd_uploads());
  if(held || busy || !(file ? games_pick_commit(pk.seq) : ra_patch_restart_commit(g))) {
    sdc_unlock();
    return;
  }
  if(ftpd_uploads())
    debugf("FTP: the restart cuts %u upload(s) in the middle", ftpd_uploads());
  if(ra_queue_in_transit() || ra_task_lboard_pending())
    debugf("RA: restart with %u unlocks and %u leaderboard results not yet out",
           ra_queue_in_transit(), ra_task_lboard_pending());
  if(file) restart_mark_file(pk.crc, board, 0);
  else     restart_mark(g, 0);
  if(board == ram_mirror_board) {
    debugf("%s: restart for %s", file ? "Games" : "RA", title);
    mcu_hw_reset();
  }
  debugf("Core switch for %s, board %u", title, board);
  if(core_switch()) mcu_hw_reset();
  // the bitstream has no switch: the game goes on, and the menu says why
  watchdog_hw->scratch[0] = 0;
  sdc_unlock();
  debugf("Core switch: the core did not reload, its bitstream does not know Z");
  start = 0;
  shown = 0;
  ftpd_hold_uploads(false);
  ftpd_stop_uploads(false);
  if(file) games_pick_cancel();
  else     ra_patch_restart_cancel();
  menu_notify(MENU_EVENT_CORE_SWITCH_FAILED);
  // the ROM now in the core decides again: a known ROM of another game of this
  // board, streamed while the switch was the target, asks for its restart. A stream
  // that still runs settles by itself when it ends
  if(!ra_patch_rom_pending()) ra_patch_settle();
}

/* The marker as the Pico found it at its start. restart_take() moves it out of
   the scratch registers before the wait for the FPGA: a boot that never reaches
   restart_rom(), because no core comes up, must not leave it for the next core
   that shows up, it would switch that one away. */
static uint32_t marker[4];

static void restart_take(void) {
  for(int i = 0; i < 4; i++) marker[i] = watchdog_hw->scratch[i];
  watchdog_hw->scratch[0] = 0;
}

/* At power-on, when no restart left a marker: the start game Save settings keeps
   (games.c) becomes the marker restart_rom() follows, a core switch first when its
   board is not the core that came up. Once per power-on: every restart of the Pico
   is a watchdog reboot, and a missing or old start game leaves the ini's ROM. */
static void start_game(void) {
  char name[FF_LFN_BUF + 1];
  games_footer_t ft;
  if((marker[0] == RESTART_MAGIC || marker[0] == RESTART_FILE) &&
     marker[2] == ~(marker[0] ^ marker[1] ^ marker[3]))
    return;
  if(watchdog_caused_reboot() || !games_start_file(name, sizeof(name), &ft)) return;
  debugf("Games: start game %s, board %u", name, ft.board);
  uint32_t crc = games_name_crc(name), b = (uint32_t)ft.board << 8;
  marker[0] = RESTART_FILE;
  marker[1] = crc;
  marker[3] = b;
  marker[2] = ~(RESTART_FILE ^ crc ^ b);
}

/* At the start, before the images are mounted: the game a restart was for. After
   a core switch the board of the core that came up decides: the game's board
   loads its ROM, another one switches on to the next core of the ring. */
static void restart_rom(void) {
  uint32_t magic = marker[0], id = marker[1], check = marker[2], hops = marker[3];
  const ra_game_t *g = NULL;
  unsigned char board;
  const char *title;
  if(check != ~(magic ^ id ^ hops)) return;
  if(magic == RESTART_MAGIC) {
    // id is the game's row in the table, several rows share an RA id
    if(!(g = ra_games_at(id))) return;
    board = g->board;
    title = g->title;
  } else if(magic == RESTART_FILE) {
    // id is the CRC of the file name, the board rides above the hops
    board = (unsigned char)(hops >> 8);
    hops &= 0xff;
    title = "the game picked";
  } else
    return;
  if(board != ram_mirror_board) {
    // a core of a third board: on to the next one, as long as the ring can be
    // longer. Without a valid header the board is unknown, nothing to compare
    if(!ram_mirror_board || hops >= SWITCH_HOPS) {
      debugf("Core switch: no core of board %u for %s after %lu switches", board, title, (unsigned long)hops);
      return;
    }
    if(g) restart_mark(g, hops + 1);
    else  restart_mark_file(id, board, hops + 1);
    for(;;) {                    /* S1 and S2 read under the lock, right before Z */
      sdc_lock();
      if(!buttons_held()) break;
      sdc_unlock();
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    debugf("Core switch for %s, board %u, on from board %u", title, board, ram_mirror_board);
    if(core_switch()) mcu_hw_reset();
    watchdog_hw->scratch[0] = 0;
    sdc_unlock();
    debugf("Core switch: the core did not reload, its bitstream does not know Z");
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

/* The server's answer to a leaderboard entry: the rank of the account's best
   value. It did not record the entry when the rank is 0, while its warning is
   on, or when the best is 0 for a result that is not. A client RetroAchievements
   has not approved gets success, the account's earlier best or 0, and the rank
   of that value, never the entry itself. */
static void banner_lboard(void) {
  static unsigned seen;
  ra_lboard_result_t r;
  char text[BANNER_LEN + 1];
  if(!ra_task_lboard_result(&seen, &r)) return;
  if(!r.rank || ra_patch_warning(NULL, 0) || (r.best == 0 && r.score != 0)) {
    banner_show("RA: LB NOT RECORDED", false, false);
    return;
  }
  snprintf(text, sizeof(text), "RA: RANK %u OF %u", r.rank, r.entries);
  banner_show(text, true, r.best == r.score);   // green mark when this entry is the new best
}

static void ra_event(const rc_runtime_event_t *ev) {
  switch(ev->type) {
  case RC_RUNTIME_EVENT_LBOARD_STARTED:
  case RC_RUNTIME_EVENT_LBOARD_CANCELED:
  case RC_RUNTIME_EVENT_LBOARD_UPDATED:
  case RC_RUNTIME_EVENT_LBOARD_TRIGGERED:
  case RC_RUNTIME_EVENT_LBOARD_DISABLED:
    ra_lboard_event(ev);
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_PRIMED:
    if(!ra_done(ev->id)) ra_primed_set(ev->id, true);
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_UNPRIMED:
  case RC_RUNTIME_EVENT_ACHIEVEMENT_RESET:
  case RC_RUNTIME_EVENT_ACHIEVEMENT_PAUSED:
  case RC_RUNTIME_EVENT_ACHIEVEMENT_DISABLED:
    ra_primed_set(ev->id, false);
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_PROGRESS_UPDATED:
    if(!ra_done(ev->id)) {
      char progress[RA_PATCH_PROGRESS_MAX];
      ra_patch_format_progress(&ra_rt, ev->id, progress, sizeof(progress));
      if(progress[0]) banner_show_progress(progress, ra_patch_title(ev->id), ra_task_hardcore());
    }
    return;
  case RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED:
    ra_primed_set(ev->id, false);
    break;
  default:
    return;
  }
  bool hardcore = ra_task_hardcore();
  if(ra_triggered < 255) ra_triggered++;
  ra_last = (unsigned char)ra_patch_index(ev->id);
  debugf("RA: %s achievement %u triggered: %s", hardcore ? "hardcore" : "softcore",
         (unsigned)ev->id, ra_patch_title(ev->id));
  // gold in hardcore, white in softcore, as on the RA site. The mark is green when
  // the unlock is new in this mode, grey when the account has it already or one
  // is queued. Asked before the queue takes this one.
  bool known = ra_state_known(ev->id) || (!hardcore && ra_state_softcore_only(ev->id));
  banner_show(ra_patch_title(ev->id), hardcore, !known);
  // the RA task keeps it on the card and sends it, this never blocks
  ra_queue_add(ev->id, hardcore);
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
      banner_show(ram_mirror_buf[4] < RAM_MIRROR_LAYOUT ? "SYS: CORE TOO OLD" : "SYS: FIRMWARE TOO OLD",
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
   game20k core of another layout, 0xE3 one with a broken header. */
static unsigned char ram_mirror_probe(void) {
  unsigned char hdr_tx[RAM_MIRROR_HEAD] = { 0 }, v = 0xE1;
  for(int i = 0; i < 50 && !ram_mirror_data; i++) {
    if(i) vTaskDelay(pdMS_TO_TICKS(10));
    v = ram_mirror_header(hdr_tx);
    if(v == 0) mcu_hw_spi_end();           /* the header is all the probe wants */
  }
  if(!ram_mirror_data)
    debugf("RAM mirror: no valid layout %u header in 50 tries, the board is unknown", RAM_MIRROR_LAYOUT);
  return ram_mirror_data ? 0 : v;
}

/* Read the header first. Byte 7 set means a harvest was running when the transfer
   started: the shadow is half old, half new, so skip it and retry on the next poll.
   That is normal, the verdict stays. From the verdict byte on, the FPGA starts no
   new harvest until chip select rises, so a clear byte 7 holds to the end. A harvest
   starting in exactly that clock tears the snapshot, the checksum (0xE6) catches it.
   Every offset in the body comes from the length the header announced. */
static void ram_mirror_poll(void) {
  unsigned char hdr_tx[RAM_MIRROR_HEAD] = { 0 };
  /* back channel for the diagnostic bars of the FPGA: the rcheevos state */
  hdr_tx[0] = ra_triggered;
  hdr_tx[1] = (unsigned char)ra_patch_count();
  hdr_tx[2] = (unsigned char)(ra_us & 0xff);
  hdr_tx[3] = (unsigned char)(ra_us >> 8);
  hdr_tx[4] = ra_last;
  hdr_tx[5] = ra_primed_any() ? 0x01 : 0x00;   /* bit 0: a challenge is on, the marker shows */

  // the banner, one character per poll in bytes 6 and 7
  banner_login();
  banner_lboard();
  restart_step();
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

  /* rcheevos only sees a snapshot that passed every check. The set comes from
     the RA task, which reads it from the card, and is activated here once it
     is there. */
  if(!ra_ready) {
    rc_runtime_init(&ra_rt);
    ra_ready = true;
  }
  /* The core counts the ends of its resets, S1 on the Nano as well as the menu's
     reset (byte 8, taken when the harvest ended). A new count is a new game: rcheevos
     starts over before it sees this snapshot. The first snapshot only sets the
     reference. Byte 9, the core's diagnostic parameters, decides whether hardcore
     is possible at all, before the first frame is evaluated. */
  /* Bytes 10 and 11 carry the complements of 8 and 9: the checksum covers only the
     body, and a wrong bit here would reset rcheevos or block hardcore for nothing.
     A pair that does not match is left out for this snapshot. */
  if(ram_mirror_buf[10] == (unsigned char)~ram_mirror_buf[8] &&
     ram_mirror_buf[11] == (unsigned char)~ram_mirror_buf[9]) {
    int resets = ram_mirror_buf[8];
    if(resets != ram_mirror_resets) {
      if(ram_mirror_resets >= 0) ra_patch_core_reset();
      ram_mirror_resets = resets;
    }
    ra_task_core_flags(ram_mirror_buf[9]);
  }
  unsigned what = ra_patch_apply_pending(&ra_rt);
  /* the SDK heap is where rcheevos keeps the set and mbedTLS a connection: a large
     set leaves less for the next request, so the log shows what is left */
  if(what & RA_PATCH_NEW_SET) debugf("RA: set active, SDK heap free %lu, FreeRTOS heap free %u",
                                     (unsigned long)getFreeHeap(), (unsigned)xPortGetFreeHeapSize());
  if(what & RA_PATCH_RESET) banner_mode_due = true;          /* a game starts */
  if(what) memset(ra_primed, 0, sizeof(ra_primed));   /* after a reset nothing is primed, a new set moves the positions */
  absolute_time_t t0 = get_absolute_time();
  rc_runtime_do_frame(&ra_rt, ra_event, ra_peek, NULL, NULL);
  int64_t dt = absolute_time_diff_us(t0, get_absolute_time());
  ra_us = dt > 65535 ? 65535 : (unsigned short)dt;

  /* the RA task pings only while frames arrive, and sends the rich presence text,
     read here where the snapshot is valid, about once a second */
  ra_task_frame();
  if(++ra_rp_frames >= RA_RP_EVERY) {
    char rp[RA_RP_MAX];
    ra_rp_frames = 0;
    rc_runtime_get_richpresence(&ra_rt, rp, sizeof(rp), ra_peek, NULL, NULL);
    ra_task_set_richpresence(rp);
    ra_patch_update_progress(&ra_rt);   /* progress and challenges for the menu's list */
  }

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

TaskHandle_t com_task_handle = NULL;

static void com_task(__attribute__((unused)) void *p ) {
  debugf("Starting main communication task");
  restart_take();             /* game20k: the marker of a restart, first of all */
  
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
    bool mismatch = false;   // a game20k core whose menu this firmware lacks: say so once the menu runs
    
    // try to load a config .xml from sd card. sys_get_config_name() always
    // returns /sd/config.xml here: the core id byte stays 0 (game20k)
    FIL fil;
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

      config_dump();
    } else {
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
      if(why == MENUS_MISMATCH) ra_task_hardcore_block(RA_HC_BLOCK_MENU, true);
      config_init();
      while(*c) xml_parse(*c++);
      config_dump();
    }

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

    // open disk images, either defaults set in sdc_init or
    // user configure ones from the ini file. This will also
    // start rom image transfers if specified in the ini file
    start_game();             /* game20k: at power-on the start game of Save settings */
    restart_rom();            /* a game picked before a restart of the Pico goes first */
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

