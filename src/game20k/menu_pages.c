/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file menu_pages.c
 *  @brief game20k's pages of the menu: the achievement list, the Games page, the dialogs and the DIP switches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mcu_hw.h"
#include "../config.h"
#include "../debug.h"
#include "../inifile.h"
#include "../menu.h"
#include "../osd.h"
#include "../sdc.h"
#include "../sysctrl.h"
#include "util/rc_version.h"
#include "games.h"
#include "menu_pages.h"
#include "menus.h"
#include "ra_patch.h"
#include "ra_queue.h"
#include "ra_state.h"
#include "ra_task.h"

// The Version dialog: what this firmware tells RetroAchievements, split over four
// lines. The version is the git describe of the game20k repository, handed in by its
// build script, so between releases it carries the distance to the last tag. The
// fourth line is the core's board id and what the ROM file is (game20k).
void menu_version_status(void) {
  char t[128];
  // a build with one of the fork's example tables knows no game or no menu, the platform
  // line says so
  snprintf(t, sizeof(t), "game20k v%s\n%s%s\nrcheevos %s\nboard %u, %s", GAME20K_VERSION, GAME20K_PLATFORM,
           strcmp(ra_games_origin, "generated") || strcmp(menus_origin, "generated") ? ", example" : "",
           RCHEEVOS_VERSION_STRING, ra_game_board(), ra_game_rom_label());
  menu_draw_dialog_for("Version", t, pdMS_TO_TICKS(8000));
}

/* ================ the achievement list ================
   One line per achievement of the active set, in the set's order: a mark, the
   title and, on the right, the progress while it runs, else the points. The
   mark is a disk when the account has it in hardcore, a ring when only in
   softcore, an exclamation mark while its challenge is on. OK on a line shows
   the details. RetroAchievements asks that progress and challenges show here. */
static const unsigned char icn_hard_bits[]   = { 0x3c,0x7e,0xff,0xff,0xff,0xff,0x7e,0x3c };
static const unsigned char icn_primed_bits[] = { 0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18 };

/* Unlocked in the mode that counts: hardcore counts in both modes, softcore only in softcore. */
static bool ra_list_done(unsigned id) {
  return ra_state_known(id) || (!ra_task_hardcore() && ra_state_softcore_only(id));
}

int ra_list_length(void) {
  return (int)ra_patch_count();
}

void ra_list_draw(void) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  int dots;

  // progress and challenges change during play: the menu timer redraws the list
  // about once a second while it shows, see menu_do()
  menu_timer_enable(true);
  menu_page_font();
  dots = u8g2_GetStrWidth(&u8g2, "...");
  if(!ra_patch_count()) {
    u8g2_DrawStr(&u8g2, 1, menu_page_y(0), "No set loaded");
    return;
  }
  // the four lines from the scroll position on, like the file selector
  for(int row = 0; row < 4; row++) {
    ra_patch_item_t it;
    int i = row + menu_page_scroll();
    if(!ra_patch_item((unsigned)i, &it)) break;
    int y = menu_page_y(row);
    bool done = ra_list_done(it.id);

    // right column: the progress while the achievement is open, else its points
    char right[RA_PATCH_PROGRESS_MAX + 4];
    if(it.progress[0] && !done) snprintf(right, sizeof(right), "%s", it.progress);
    else                        snprintf(right, sizeof(right), "%u", it.points);
    int rw = u8g2_GetStrWidth(&u8g2, right);

    // the title between mark and right column, cut with "..." when too long
    char title[RA_PATCH_TITLE_MAX + 3];
    int room = width - menu_page_x() - rw - 3;
    snprintf(title, sizeof(title), "%s", it.title);
    if(u8g2_GetStrWidth(&u8g2, title) > room) {
      while(title[0] && u8g2_GetStrWidth(&u8g2, title) > room - dots) title[strlen(title) - 1] = 0;
      strcat(title, "...");
    }

    if(it.primed && !done)             u8g2_DrawXBM(&u8g2, 1, y - 8, 8, 8, icn_primed_bits);
    else if(ra_state_known(it.id))     u8g2_DrawXBM(&u8g2, 1, y - 8, 8, 8, icn_hard_bits);
    else if(ra_state_softcore_only(it.id)) u8g2_DrawXBM(&u8g2, 1, y - 8, 8, 8, icn_off_bits);
    u8g2_DrawStr(&u8g2, menu_page_x(), y, title);
    u8g2_DrawStr(&u8g2, width - rw - 1, y, right);

    if(menu_page_selected() == i + 1)
      u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
  }
}

/* Cuts s so that it plus "..." fits width in the current font, and adds "...".
   s needs room for three more characters. */
static void ra_list_ellipsize(char *s, int width) {
  int dots = u8g2_GetStrWidth(&u8g2, "...");
  if(u8g2_GetStrWidth(&u8g2, s) <= width) return;
  while(s[0] && u8g2_GetStrWidth(&u8g2, s) > width - dots) s[strlen(s) - 1] = 0;
  while(strlen(s) && s[strlen(s) - 1] == ' ') s[strlen(s) - 1] = 0;
  strcat(s, "...");
}

/* Breaks text into at most max lines of the display width at spaces, into out,
   the way menu_wrap_text() breaks a dialog. When text remains, the last line ends
   with "...". Returns the number of lines. */
static int ra_list_wrap(const char *text, char out[][80], int max) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  int n = 0;
  menu_page_font();
  while(*text && n < max) {
    size_t take = 0, gap = 0, i;
    char *line = out[n];
    while(*text == ' ') text++;
    if(!*text) break;
    for(i = 0; text[i] && i < 80 - 4; i++) {
      line[i] = text[i];
      line[i + 1] = 0;
      if(u8g2_GetStrWidth(&u8g2, line) > width) break;
      if(text[i] == ' ') gap = i;
      take = i + 1;
    }
    if(text[i] && text[i] != ' ' && gap) take = gap;
    if(!take) take = 1;
    line[take] = 0;
    text += take;
    n++;
    while(*text == ' ') text++;
    if(n == max && *text) ra_list_ellipsize(line, width);   // more text than lines
  }
  return n;
}

/* The details of one achievement: its title as the heading, the description in
   up to three lines, then points and where it stands. Four lines fit the display
   under a heading. */
void ra_list_select(int line) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  ra_patch_item_t it;
  char head[RA_PATCH_TITLE_MAX + 3], stat[48], desc[3][80], t[4 * 80];
  const char *state;
  int n, i;

  if(!ra_patch_item((unsigned)line, &it)) return;
  // where it stands: done in this mode first, else what is going on
  if(ra_state_known(it.id))                                state = "hardcore";
  else if(ra_state_softcore_only(it.id) && !ra_task_hardcore()) state = "softcore";
  else if(it.primed)                                       state = "challenge on";
  else if(it.progress[0])                                  state = it.progress;
  else if(ra_state_softcore_only(it.id))                   state = "softcore only";
  else                                                     state = "open";

  // the heading in the dialog's bold font, cut to the display
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
  snprintf(head, sizeof(head), "%s", it.title);
  ra_list_ellipsize(head, width);

  menu_page_font();
  snprintf(stat, sizeof(stat), "%u points, %s", it.points, state);
  if(u8g2_GetStrWidth(&u8g2, stat) > width) snprintf(stat, sizeof(stat), "%u pts, %s", it.points, state);
  n = ra_list_wrap(it.desc, desc, 3);
  t[0] = 0;
  for(i = 0; i < n; i++) { strcat(t, desc[i]); strcat(t, "\n"); }
  strcat(t, stat);
  menu_draw_dialog_for(head, t, pdMS_TO_TICKS(10000));
}

/* ================ the Games page ================
   One line per game on the card, by title (games.c). OK on a line starts it: a game
   of this board streams its file to the core in place, one of another board needs a
   core switch, see switch_step() in game_switch.c. The list is read when the page opens. A
   later change of the card is read before the menu task draws the page next, before a
   key acts on it and before the OSD comes on, see menu_do(). */
int games_list_length(void) {
  return (int)games_count();
}

/* The scroll that shows line sel (lines count from 1), as stepping down to it would
   scroll, see menu_entry_go() */
int games_list_scroll(int sel) {
  int entries = (int)games_count() + 1;
  return (entries <= 5 || sel <= 3) ? 0 : (sel < entries - 2) ? sel - 3 : entries - 5;
}

/* The line of the game whose file name has this CRC, lines count from 1, 0 for none */
static int games_list_line(uint32_t crc) {
  games_item_t it;
  for(unsigned i = 0; games_item(i, &it); i++)
    if(it.crc == crc) return (int)i + 1;
  return 0;
}

/* Reads the changed card, menu task only. The selection stays on its game. If that game
   is gone, the selection keeps its line as far as the list reaches. It stays in view, and
   the first game on an empty page gets it, as on opening. True when the selection marks
   another game than the page showed, or a game where it marked the title. */
bool games_list_reread(void) {
  games_item_t it;
  // the key repeat draws from the timer task and must not walk the list while it is read
  menu_stop_repeat();
  uint32_t crc = 0;
  int was = (int)games_count(), before = menu_page_selected(), sel = before, scroll = menu_page_scroll();
  bool on_game = sel > 0 && games_item((unsigned)sel - 1, &it);
  if(on_game) crc = it.crc;
  games_read();
  int line = on_game ? games_list_line(crc) : 0;
  if(line) sel = line;
  int n = (int)games_count();
  if(sel > n) sel = n;
  if(!was && n) sel = 1;
  if(sel < scroll + 1 || sel > scroll + 4 || scroll > (n > 4 ? n - 4 : 0))
    scroll = games_list_scroll(sel);
  menu_page_move(sel, scroll);
  return on_game ? !line : sel != before;
}

void games_list_draw(void) {
  const int width = u8g2_GetDisplayWidth(&u8g2);
  menu_page_font();
  if(!games_count()) {
    u8g2_DrawStr(&u8g2, 1, menu_page_y(0), "No games on the card");
    return;
  }
  for(int row = 0; row < 4; row++) {
    games_item_t it;
    char title[GAMES_TITLE_MAX + 4];
    int i = row + menu_page_scroll();
    if(!games_item((unsigned)i, &it)) break;
    int y = menu_page_y(row);
    snprintf(title, sizeof(title), "%s", it.title);
    ra_list_ellipsize(title, width - 2);
    u8g2_DrawStr(&u8g2, 1, y, title);
    if(menu_page_selected() == i + 1)
      u8g2_DrawButtonFrame(&u8g2, 0, y, U8G2_BTN_INV, width, 1, 1);
  }
}

void games_list_select(int line) {
  games_item_t it;
  games_pick_t pk;
  const char *cwd = sdc_get_cwd(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  const char *img = sdc_get_image_name(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  if(!games_item((unsigned)line, &it)) return;
  // the file that runs already, with no restart pending: nothing to start, the menu closes
  if(img && cwd && !strcmp(cwd, CARD_MOUNTPOINT) && games_name_crc(img) == it.crc &&
     !ra_patch_switch_to() && !games_picked(&pk)) {
    osd_enable(OSD_INVISIBLE);
    return;
  }
  // game_switch.c takes it from here. A game of this board streams at once and the menu
  // closes onto it, a core switch says so, see MENU_EVENT_CORE_SWITCH
  games_pick(&it);
  if(it.board == ra_game_board()) {
    osd_enable(OSD_INVISIBLE);
    return;
  }
  menu_pop();
}

/* Reads the card, the line to open the page on: the game that runs, else the first one. */
int games_list_first(void) {
  const char *cwd = sdc_get_cwd(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  const char *img = sdc_get_image_name(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  int sel = 0;
  games_read();
  // the running file's line, as games_list_select() tells it; lines count from 1
  if(img && cwd && !strcmp(cwd, CARD_MOUNTPOINT))
    sel = games_list_line(games_name_crc(img));
  if(!sel && games_count()) sel = 1;
  return sel;
}

/* game20k: the list with this id in the menu or one below it, NULL when there is none */
static const config_list_t *menu_find_list(const config_menu_t *menu, char id) {
  for(const config_menu_entry_t *me = menu ? menu->entries : NULL; me; me = me->next) {
    if(me->type == CONFIG_MENU_ENTRY_MENU) {
      const config_list_t *l = menu_find_list(me->menu, id);
      if(l) return l;
    } else if(me->type == CONFIG_MENU_ENTRY_LIST && me->list->id == (unsigned char)id)
      return me->list;
  }
  return NULL;
}

/* game20k: the first DIP switch the running game's set expects at another value,
   as the list's label and the label of the value it expects, e.g. "Bonus: 10K 40K+".
   False when every switch has that value. A switch this core does not have cannot
   be wrong. */
static bool menu_dip_wanted(char *buf, size_t size) {
  unsigned n, i;
  const ra_dip_t *dip = ra_game_dips(&n);
  for(i = 0; dip && i < n; i++) {
    if(!menu_variable_exists(dip[i].id) || menu_variable_get(dip[i].id) == dip[i].value) continue;
    const config_list_t *l = cfg ? menu_find_list(cfg->menu, dip[i].id) : NULL;
    const config_listentry_t *le = l ? l->listentries : NULL;
    while(le && le->value != dip[i].value) le = le->next;
    snprintf(buf, size, "%s %s", l ? l->label : "DIP", le ? le->label : "?");
    return true;
  }
  return false;
}

/* game20k: true when every DIP switch the running game's set expects has that value */
bool menu_dips_as_expected(void) {
  return !menu_dip_wanted(NULL, 0);
}

/* game20k: "Set for RA" in the DIP switches. Every switch the running game's set
   expects goes to that value, and a change resets the game as a change by hand
   does. Save settings keeps them. */
void menu_dips_set(void) {
  unsigned n, i, changed = 0;
  const ra_dip_t *dip = ra_game_dips(&n);
  for(i = 0; dip && i < n; i++)
    if(menu_variable_exists(dip[i].id) && menu_variable_get(dip[i].id) != dip[i].value) {
      menu_variable_set(dip[i].id, dip[i].value);
      changed++;
    }
  if(changed) {
    config_action_t *reset = config_get_action("reset");
    if(reset) sys_run_action(reset);
  }
  menu_draw_dialog("DIP switches", !dip ? "This game's set expects no DIP switches." :
                   changed ? "Set as the game's achievement set expects." : "Already as the set expects.");
}

// The Account dialog: the account and where it stands, the count, and the
// most urgent note. Four lines fit the display.

void menu_ra_status(void) {
  char t[100], who[24], count[32], note[32], want[28];
  const char *user = inifile_config_has("ra", "user") ? inifile_config_get_str("ra", "user") : "-";
  const char *state;
  const int width = u8g2_GetDisplayWidth(&u8g2);

  // the name on one line: a wide one is cut with "..", so the note below stays
  // on the display
  snprintf(who, sizeof(who), "%s", user);
  menu_page_font();
  for(size_t len = strlen(who); len > 2 && u8g2_GetStrWidth(&u8g2, who) > width; len--) {
    who[len - 1] = '\0';
    who[len - 2] = '.';
    who[len - 3] = '.';
  }

  switch(ra_task_state()) {
  case RA_TASK_LOGGED_IN:  state = "logged in";      break;
  case RA_TASK_REJECTED:   state = "login rejected"; break;
  case RA_TASK_NO_ACCOUNT: state = "no account";     break;
  case RA_TASK_NO_TIME:    state = "no time server"; break;
  case RA_TASK_RETRYING:   state = "offline";        break;
  case RA_TASK_NO_GAME:    state = "no game";        break;
  default:                 state = "connecting";     break;
  }
  // the mode in front, it decides how the unlocks count. Softcore although the
  // menu asks for hardcore names the reason instead of the login state. A wrong
  // game comes before an unknown ROM, it also explains one (game20k).
  char line[40];
  unsigned why = ra_task_hardcore_blocked();
  // SET without a login: the login state says more than "till online"
  if((why & ~RA_HC_BLOCK_SET) == 0 && (ra_task_state() == RA_TASK_REJECTED || ra_task_state() == RA_TASK_NO_ACCOUNT))
    why = 0;
  if(!ra_task_hardcore() && ra_task_hardcore_wanted() && why)
    snprintf(line, sizeof(line), "softcore: %s",
             (why & RA_HC_BLOCK_CORE) ? "test core" : (why & RA_HC_BLOCK_XML) ? "config.xml" :
             (why & RA_HC_BLOCK_MENU) ? "mismatch" :
             (why & RA_HC_BLOCK_KEY)  ? "key error" : (why & RA_HC_BLOCK_SIZE) ? "set too big" :
             (why & RA_HC_BLOCK_GAME) ? "wrong game" :
             (why & RA_HC_BLOCK_ROM)  ? "ROM unknown" : "till online");
  else
    snprintf(line, sizeof(line), "%s, %s", ra_task_hardcore() ? "hardcore" : "softcore", state);
  // the counts refer to the active set, without one there is nothing to count
  if(!ra_patch_count())
    snprintf(count, sizeof(count), "no set loaded");
  else if(ra_state_softcore_count())
    snprintf(count, sizeof(count), "%u of %u, %u softcore", ra_state_count(), ra_patch_count(), ra_state_softcore_count());
  else
    snprintf(count, sizeof(count), "%u of %u unlocked", ra_state_count(), ra_patch_count());

  // the note: a DIP switch the set does not expect comes first, its conditions
  // would never fire, then what still waits for the server, then in hardcore
  // the server's warning, e.g. "Unknown Emulator": until RetroAchievements
  // approves this client, it keeps the unlocks as casual.
  if(menu_dip_wanted(want, sizeof(want))) snprintf(note, sizeof(note), "Set %s", want);
  else if(ra_queue_pending()) snprintf(note, sizeof(note), "%u to send", ra_queue_pending());
  else if(!ra_task_hardcore() || !ra_patch_warning(note, sizeof(note))) note[0] = 0;

  snprintf(t, sizeof(t), "%s\n%s\n%s\n%s", who, line, count, note);
  // the game's title as the heading, the four lines are taken (game20k).
  // "Account" while no table game is known.
  menu_draw_dialog_for(ra_game_title() ? ra_game_title() : "Account", t, pdMS_TO_TICKS(8000));
}

/* Before a ROM file picked under "ROM set" goes to the core. True when it goes no further
   here: an old file without a footer, or the ROM file of a known game of another board,
   e.g. galaga.rom in the Pac-Man core, which needs that board's core: game_switch.c
   switches the core and starts the game there, see switch_step(). */
bool menu_rom_pick(int drive, const char *name) {
  if(drive != MAX_DRIVES + RA_PATCH_ROM_IMAGE) return false;
  // a file without a footer is an old one, it neither switches nor streams
  games_footer_t ft;
  char path[strlen(sdc_get_cwd(drive)) + strlen(name) + 2];
  sprintf(path, "%s/%s", sdc_get_cwd(drive), name);
  if(!games_footer_of(path, &ft)) {
    debugf("IMG %d: %s has no valid footer, an old ROM file", drive - MAX_DRIVES, path);
    menu_notify(MENU_EVENT_ROM_OLD);
    return true;
  }
  return ra_patch_pick_other_board(name);
}

/* The events of the pages, true when cmd is one of them. Menu task. */
bool menu_page_event(long cmd) {
  if(cmd == MENU_EVENT_CORE_SWITCH) {
    // game20k: the game picked runs on another board, game_switch.c loads its core in
    // a few seconds. A game picked on the Games page goes first. A game of this
    // board needs no core switch, it streams in place
    const ra_game_t *g = ra_patch_switch_to();
    games_pick_t pk;
    char message[80];
    if(games_picked(&pk)) {
      snprintf(message, sizeof(message), "%s: loading its core", pk.title);
      menu_draw_dialog_for("Games", message, pdMS_TO_TICKS(4000));
    } else if(g) {
      snprintf(message, sizeof(message), "%s: loading its core", g->title);
      menu_draw_dialog_for("Core switch", message, pdMS_TO_TICKS(4000));
    }
    return true;
  }
  if(cmd == MENU_EVENT_ROM_OLD) {
    // game20k: a ROM file from before the footer, nothing went to the core
    menu_draw_dialog_for("Old ROM file", "Rebuild the card\nwith make_sdcard.sh", pdMS_TO_TICKS(6000));
    return true;
  }
  if(cmd == MENU_EVENT_ROM_DAMAGED) {
    // game20k: the core stays in reset, the player picks another game
    menu_draw_dialog_for("ROM file damaged", "Copy the file again.\nThe game does not start.", pdMS_TO_TICKS(6000));
    menu_games_show();
    return true;
  }
  if(cmd == MENU_EVENT_GAMES) {
    // game20k: the core's ROM file is missing, a game is picked instead
    menu_games_show();
    return true;
  }
  if(cmd == MENU_EVENT_GAMES_CHANGED) {
    // game20k: the card changed. The page on screen draws again, and menu_do() reads the
    // card first. A message on top or a hidden OSD waits for the page's next draw
    if(games_page_shown() && osd_is_visible() && games_stale())
      menu_do(MENU_EVENT_NONE);
    return true;
  }
  if(cmd == MENU_EVENT_MISMATCH) {
    // game20k: core and firmware come from different releases, the basic menu runs
    menu_draw_dialog_for("Release mismatch", "Firmware and FPGA\ndo not match.\nUpdate both.",
                         pdMS_TO_TICKS(8000));
    return true;
  }
  if(cmd == MENU_EVENT_CORE_SWITCH_FAILED) {
    // game20k: the running bitstream was built before the core switch
    menu_draw_dialog_for("Core switch", "This bitstream cannot load\nanother core. The game goes on.",
                         pdMS_TO_TICKS(5000));
    return true;
  }
  return false;
}
