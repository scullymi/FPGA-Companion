/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file banner.c
 *  @brief The banner at the bottom of the picture: its queue and what it says about the account and the mode. */
#include <stdio.h>
#include <string.h>

#include "../mcu_hw.h"
#include "../debug.h"
#include "../inifile.h"
#include "../menu.h"
#include "banner.h"
#include "game.h"
#include "ra.h"

/* The banner at the bottom of the picture: 24 characters the FPGA shows for a
   while. It carries the title of an unlock, or a message that starts with who
   speaks, "RA: " for the achievements and "SYS: " for the machine itself. One
   character goes over with each mirror poll in header bytes 6 and 7. The show
   flag follows once the whole text is in the FPGA, so it appears at once. Banners
   that arrive while one is shown wait their turn, a few of them. */
#define BANNER_LEN         24
#define BANNER_MS          8000
#define BANNER_QUEUE       4
#define BANNER_PROGRESS_MS 2500         /* a progress banner is short, the next one may follow soon */
#define BANNER_DIP "RA: DIP NOT AS SET" /* game20k: a DIP switch the set does not expect, Account names it */
struct banner {
  char text[BANNER_LEN];
  bool gold;   /* text gold for hardcore, white for softcore, as on the RA site */
  bool new;    /* mark green: new, goes to the server. Grey: the account has it, or it is queued */
  unsigned ms; /* how long it shows */
  bool mode;   /* the mode banner, or the server's warning after it, see banner_login() */
  bool keep;   /* game20k: about the device or the account, a game switch keeps it, see banner_clear() */
};
static banner_t banner;                 /* the one being sent or shown */
static banner_t banner_q[BANNER_QUEUE]; /* the ones waiting, oldest first */
static unsigned banner_n;               /* how many wait */
static unsigned banner_pos;             /* next character to send */
static bool banner_pending;             /* text on its way, show follows after the pass */
static bool banner_live;                /* show flag on */
static TickType_t banner_start;         /* when the show flag went on */
static bool banner_mode_due;            /* a game started or the mode changed: say which mode counts */
static banner_t banner_progress;        /* the latest progress, shown only when no other banner waits */
static bool banner_progress_due;

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
  b->mode = false;
  b->keep = false;
}

/* Queues a banner, returns its place in the queue, NULL when the queue is full. */
banner_t *banner_show(const char *text, bool gold, bool new) {
  banner_t b;
  if(banner_n >= BANNER_QUEUE) {
    debugf("RA: banner '%s' dropped, %u already wait", text, banner_n);
    return NULL;
  }
  banner_fill(&b, text, gold, new, BANNER_MS);
  banner_q[banner_n++] = b;
  debugf("RA: banner '%.*s' for %u s (%s text, %s mark)%s", BANNER_LEN, b.text, BANNER_MS / 1000,
         gold ? "gold" : "white", new ? "green" : "grey", banner_n > 1 ? ", waits" : "");
  return &banner_q[banner_n - 1];
}

/* game20k: a banner about the device or the account, a game switch keeps it */
banner_t *banner_show_kept(const char *text, bool gold, bool new) {
  banner_t *q = banner_show(text, gold, new);
  if(q) q->keep = true;
  return q;
}

/* game20k: another game runs, see banner_follow_game(). The banners of the game before
   go, the one shown and those that wait. One about the device or the account stays. */
static void banner_clear(void) {
  unsigned n = 0;
  for(unsigned i = 0; i < banner_n; i++)
    if(banner_q[i].keep) banner_q[n++] = banner_q[i];
  banner_n            = n;
  banner_progress_due = false;
  if((banner_live || banner_pending) && !banner.keep) {
    banner_live    = false;
    banner_pending = false;
    banner_pos     = 0;
  }
}

/* game20k: clears the banners once another game runs. A settle comes between two
   polls, so at the start of a poll what waits was queued for the game before. */
void banner_follow_game(void) {
  static unsigned gen; /* the game generation the banners are for */
  if(gen == game_generation()) return;
  gen = game_generation();
  banner_clear();
}

/* The progress of an achievement as a short banner, "12/50 TITLE". Only the
   latest counts: it waits until no other banner shows or waits, and a newer one
   replaces it. RetroAchievements asks that progress shows during play. */
void banner_show_progress(const char *progress, const char *title, bool gold) {
  char text[BANNER_LEN + 1];
  size_t n = strlen(progress);
  // the FPGA font has no %, a percentage reads "37 PCT"
  if(n && progress[n - 1] == '%') snprintf(text, sizeof(text), "%.*s PCT %s", (int)(n - 1), progress, title);
  else snprintf(text, sizeof(text), "%s %s", progress, title);
  banner_fill(&banner_progress, text, gold, false, BANNER_PROGRESS_MS);
  banner_progress_due = true;
}

/* One poll's worth of banner work: ends a banner after its time, starts the
   next waiting one, then a waiting progress, and fills header bytes 6 and 7.
   Bit 7 show, bit 6 gold text, bit 5 green mark, bits 4..0 the position, byte 7
   the character. */
void banner_step(unsigned char *hdr) {
  if(banner_live && (xTaskGetTickCount() - banner_start) >= pdMS_TO_TICKS(banner.ms)) banner_live = false;
  if(!banner_live && !banner_pending && banner_n) {
    banner = banner_q[0];
    memmove(banner_q, banner_q + 1, (--banner_n) * sizeof(banner_q[0]));
    banner_pos     = 0;
    banner_pending = true;
  } else if(!banner_live && !banner_pending && banner_progress_due) {
    banner              = banner_progress;
    banner_progress_due = false;
    banner_pos          = 0;
    banner_pending      = true;
  }
  hdr[6] = (unsigned char)((banner_live ? 0x80 : 0) | (banner.gold ? 0x40 : 0) | (banner.new ? 0x20 : 0) |
                           (banner_pos & 0x1f));
  hdr[7] = (unsigned char)banner.text[banner_pos];
  if(++banner_pos >= BANNER_LEN) {
    banner_pos = 0;
    if(banner_pending) { // the whole text is over, now it may show
      banner_pending = false;
      banner_live    = true;
      banner_start   = xTaskGetTickCount();
    }
  }
}

#if GAME20K_RA
/* game20k: true when a banner with this text shows, is on its way or waits */
static bool banner_has(const char *text) {
  banner_t b;
  banner_fill(&b, text, false, false, 0);
  if((banner_live || banner_pending) && !memcmp(banner.text, b.text, BANNER_LEN)) return true;
  for(unsigned i = 0; i < banner_n; i++)
    if(!memcmp(banner_q[i].text, b.text, BANNER_LEN)) return true;
  return false;
}

/* game20k: takes the banners with this text that still wait out of the queue */
static void banner_drop_text(const char *text) {
  banner_t b;
  unsigned n = 0;
  banner_fill(&b, text, false, false, 0);
  for(unsigned i = 0; i < banner_n; i++)
    if(memcmp(banner_q[i].text, b.text, BANNER_LEN)) banner_q[n++] = banner_q[i];
  banner_n = n;
}

/* game20k: takes the mode banners that still wait out of the queue, see banner_login() */
static void banner_drop_mode(void) {
  unsigned n = 0;
  for(unsigned i = 0; i < banner_n; i++)
    if(!banner_q[i].mode) banner_q[n++] = banner_q[i];
  banner_n = n;
}

/* A message when the RA task's state changes: the account is in, or the
   player should know why nothing counts yet. Then the mode, at each game start
   and when it changes, as RetroAchievements asks: the player sees what counts. */
void banner_login(void) {
  static ra_task_state_t shown = RA_TASK_STARTING;
  static bool offline_told;   /* the offline message once, until the login */
  static int mode_shown = -1; /* the mode the last mode banner named, -1 before the first */
  ra_task_state_t now   = ra_task_state();
  int mode              = ra_task_hardcore();
  // game20k: the "no game" between a core switch and the settle of the new core's
  // ROM is no news, nor is the one the RA task still reports once a game with an
  // id runs. Left out, shown stays, and the login after it is not told twice
  bool passing = now == RA_TASK_NO_GAME && (game_new_core_pending() || game_id());
  if(now != shown && !passing) {
    shown = now;
    // messages are white, the mark is green when all is well and grey when not
    if(now == RA_TASK_LOGGED_IN) {
      char text[BANNER_LEN + 1];
      snprintf(text, sizeof(text), "RA: %s", inifile_config_get_str("ra", "user"));
      banner_show_kept(text, false, true);
    } else if(now == RA_TASK_REJECTED) banner_show_kept("RA: LOGIN REJECTED", false, false);
    else if(now == RA_TASK_NO_TIME) banner_show_kept("RA: NO TIME SERVER", false, false);
    else if(now == RA_TASK_NO_GAME) banner_show("RA: NO GAME", false, false);
    else if(now == RA_TASK_RETRYING && !offline_told) {
      // no server: achievements still count, their unlocks wait on the card
      offline_told = true;
      banner_show_kept("RA: OFFLINE UNLOCKS KEPT", false, false);
    }
  }
  // a change against the mode the player was told asks again. Before the first
  // banner there is none: at the boot the settle lifts the blocks of ROM, game and
  // set, and the first game start names the mode that results.
  if(mode_shown >= 0 && mode != mode_shown) banner_mode_due = true;
  // once the task knows whether there is an account: without one nothing counts.
  // Softcore although the menu asks for hardcore names the reason. A game start
  // that switches to hardcore also changes the mode: both set the one flag before
  // it is read here, the banner shows once. While a ROM streams to the core the
  // banner waits: the stream blocks hardcore until its digest is checked, a second
  // later, and the settle after it names the mode that counts. It also waits through
  // a game switch, whose order is open: the passing "no game", which the RA task
  // leaves only once the new game is known, and hardcore asked for but not yet on,
  // which applies when the game's reset ends. Either would say the wrong mode or
  // spend the banner. So does the new game's set while the RA task still reads it
  // from the card: its block holds until then, the banner would say softcore.
  if(banner_mode_due && now != RA_TASK_STARTING && !passing && !ra_task_hardcore_due() &&
     !game_rom_pending() && !ra_patch_set_pending()) {
    unsigned why = ra_task_hardcore_blocked();
    const char *text =
        mode                         ? "RA: HARDCORE"
        : !ra_task_hardcore_wanted() ? "RA: SOFTCORE"
        : (why & RA_HC_BLOCK_CORE)   ? "RA: SOFTCORE TEST CORE"
        : (why & RA_HC_BLOCK_XML)    ? "RA: SOFTCORE CONFIG.XML"
        : (why & RA_HC_BLOCK_MENU)   ? "RA: SOFTCORE MISMATCH"
        : (why & RA_HC_BLOCK_KEY)    ? "RA: SOFTCORE KEY ERROR"
        : (why & RA_HC_BLOCK_SIZE)   ? "RA: SOFTCORE SET TOO BIG"
                                     :
                                     // a wrong game also explains an unknown ROM, so it comes first
            (why & RA_HC_BLOCK_GAME) ? "RA: SOFTCORE WRONG GAME"
        : (why & RA_HC_BLOCK_ROM)    ? "RA: SOFTCORE ROM UNKNOWN"
                                     :
                                     // without a login the set never comes, the login banner says why
            (why & RA_HC_BLOCK_SET) && now != RA_TASK_REJECTED ? "RA: SOFTCORE TILL ONLINE"
                                                               : "RA: SOFTCORE";
    banner_mode_due = false;
    mode_shown      = mode;
    // without an account or a game nothing counts, their own banner says so
    if(now != RA_TASK_NO_ACCOUNT && now != RA_TASK_NO_GAME) {
      char warn[RA_PATCH_TITLE_MAX], line[4 + RA_PATCH_TITLE_MAX];
      banner_t b, *q;
      // game20k: a mode banner that shows with the same text says it already, else
      // the newest mode replaces the mode banners still waiting. Quick resets, a
      // list with a reset action or the two resets of a game switch, queued the
      // same lines again and again
      banner_fill(&b, text, mode, true, BANNER_MS);
      bool told = (banner_live || banner_pending) && banner.mode && !memcmp(banner.text, b.text, BANNER_LEN);
      bool dips = menu_dips_as_expected();
      // a DIP banner that still waits, from the game before or from before the switch was
      // set right, is no longer true
      if(dips) banner_drop_text(BANNER_DIP);
      if(told && !dips && !banner_has(BANNER_DIP) && (q = banner_show(BANNER_DIP, false, false)))
        q->mode = true; /* the mode is said, a switch changed since */
      if(!told) {
        banner_drop_mode();
        if((q = banner_show(text, mode, true))) q->mode = true;
        // in hardcore the server's warning follows, as rc_client shows it. While
        // RetroAchievements has not approved this client, it keeps the unlocks as
        // casual, and the player sees that next to the mode the device plays in.
        if(q && mode && ra_patch_warning(warn, sizeof(warn))) {
          snprintf(line, sizeof(line), "RA: %s", warn);
          if((q = banner_show(line, false, false))) q->mode = true;
        }
        // game20k: a DIP switch the set does not expect, its conditions may never
        // fire. Said at each game start, a change of a switch resets the core
        if(q && !dips && (q = banner_show(BANNER_DIP, false, false))) q->mode = true;
      }
    }
  }
}

/* The server's answer to a leaderboard entry: the rank of the account's best
   value. It did not record the entry when the rank is 0, while its warning is
   on, or when the best is 0 for a result that is not. A client RetroAchievements
   has not approved gets success, the account's earlier best or 0, and the rank
   of that value, never the entry itself. */
void banner_lboard(void) {
  static unsigned seen;
  ra_lboard_result_t r;
  char text[BANNER_LEN + 1];
  if(!ra_task_lboard_result(&seen, &r)) return;
  // game20k: a result reached in another game is not the running game's news
  if(strcmp(r.hash, game_hash())) return;
  if(!r.rank || ra_patch_warning(NULL, 0) || (r.best == 0 && r.score != 0)) {
    banner_show("RA: LB NOT RECORDED", false, false);
    return;
  }
  snprintf(text, sizeof(text), "RA: RANK %u OF %u", r.rank, r.entries);
  banner_show(text, true, r.best == r.score); // green mark when this entry is the new best
}

#endif /* GAME20K_RA */

/* The mode is said again at the next chance, see banner_login(). */
void banner_ask_mode(void) { banner_mode_due = true; }

/* A banner on its way or shown starts over, a new core has none of it. */
void banner_restart(void) {
  if(banner_live || banner_pending) {
    banner_live    = false;
    banner_pending = true;
    banner_pos     = 0;
  }
}
