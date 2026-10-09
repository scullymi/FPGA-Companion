/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file games.c
 *  @brief The Games page's list, the game picked there and the start game.
 *
 *  The list is every *.rom in the card's root whose footer is valid (games_file.c)
 *  and names a board with a menu in this firmware (menus.h), sorted by title. It is read
 *  in the menu task when the page opens and when the card changed, one footer per
 *  file, and holds only titles and names. The content is checked when it streams
 *  to the core: sdc.c compares the SHA-256 the stream yields with the footer's, a
 *  file that fails stays off the list until the card changes.
 *
 *  A game picked on the page reaches main.c as the CRC-32 of its file name and its
 *  board: restart_step() restarts the Pico, with a core switch for another board,
 *  and restart_rom() finds the file again by that CRC. The start game is a file
 *  name in GAMES_START_FILE: config.ini belongs to the user and holds secrets, the
 *  ini of a core is read only by that core, so the start game has a file of its own
 *  that every core reads at power-on. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <FreeRTOS.h>
#include <task.h>
#include <ff.h>
#include "debug.h"
#include "sdc.h"
#include "menus.h"      // the boards this firmware has a core menu for
#include "ra_patch.h"   // RA_PATCH_ROM_IMAGE
#include "games.h"

#define GAMES_PATH_MAX  (FF_LFN_BUF + 6)   /**< "/sd/" plus a long file name plus the NUL */
#define GAMES_DAMAGED_MAX 4                 /**< damaged files remembered, the oldest gives way */

/* One line of the list: the title, then the file name, in one allocation. */
typedef struct games_entry {
  struct games_entry *next;
  uint32_t            crc;     // of the file name
  unsigned char       board;
  const char         *name;    // behind the title in text[]
  char                text[];  // title NUL name NUL
} games_entry_t;

static games_entry_t *list;            // menu task only
static unsigned       list_n;
static volatile bool  stale = true;    // the card changed, any task sets it
static uint32_t       damaged[GAMES_DAMAGED_MAX];   // CRCs of name, 0 for none, under a critical section
static unsigned       damaged_next;
static games_pick_t   pick;            // under a critical section
static bool           pick_set, pick_committed;

uint32_t games_name_crc(const char *name) {
  return games_crc32(name, strlen(name));
}

bool games_stale(void) { return stale; }

void games_changed(const char *name) {
  // only the root's ROM files are listed, but a file moved there counts as well
  if(name && *name) {
    size_t len = strlen(name);
    if(len < 4 || strcasecmp(name + len - 4, ".rom")) return;
  }
  taskENTER_CRITICAL();
  memset(damaged, 0, sizeof(damaged));
  stale = true;
  taskEXIT_CRITICAL();
}

void games_damaged(const char *name) {
  uint32_t crc = games_name_crc(name);
  taskENTER_CRITICAL();
  damaged[damaged_next] = crc;
  damaged_next = (damaged_next + 1) % GAMES_DAMAGED_MAX;
  stale = true;
  taskEXIT_CRITICAL();
}

static bool is_damaged(uint32_t crc) {
  bool hit = false;
  taskENTER_CRITICAL();
  for(unsigned i = 0; i < GAMES_DAMAGED_MAX; i++) if(damaged[i] && damaged[i] == crc) hit = true;
  taskEXIT_CRITICAL();
  return hit;
}

/* Puts one file into the list at its place by title. */
static void insert(const char *title, const char *name, uint32_t crc, unsigned char board) {
  size_t tl = strlen(title), nl = strlen(name);
  games_entry_t *e = pvPortMalloc(sizeof(*e) + tl + nl + 2), **at = &list;
  if(!e) return;
  memcpy(e->text, title, tl + 1);
  memcpy(e->text + tl + 1, name, nl + 1);
  e->name  = e->text + tl + 1;
  e->crc   = crc;
  e->board = board;
  while(*at && games_order((*at)->text, (*at)->name, title, name) <= 0) at = &(*at)->next;
  e->next = *at;
  *at = e;
  list_n++;
}

void games_read(void) {
  DIR dir;
  FILINFO fno;
  FIL f;
  games_footer_t ft;
  char path[GAMES_PATH_MAX], title[GAMES_TITLE_MAX + 1];
  unsigned old = 0, foreign = 0, bad = 0;

  while(list) {
    games_entry_t *next = list->next;
    vPortFree(list);
    list = next;
  }
  list_n = 0;
  stale  = false;   // before the read: a change while it runs reads again
  sdc_lock();
  if(f_opendir(&dir, CARD_MOUNTPOINT) == FR_OK) {
    while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
      size_t len = strlen(fno.fname);
      if((fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) || len < 5 || strcasecmp(fno.fname + len - 4, ".rom"))
        continue;
      snprintf(path, sizeof(path), "%s/%s", CARD_MOUNTPOINT, fno.fname);
      bool ok = f_open(&f, path, FA_READ) == FR_OK;
      if(ok) {
        ok = games_footer_read(&f, &ft);
        f_close(&f);
      }
      // a file without a valid footer is an old one (or none at all), a board
      // without a menu here has no core in this firmware's ring
      if(!ok)                             { old++;     continue; }
      if(!menus_by_board(ft.board, NULL)) { foreign++; continue; }
      uint32_t crc = games_name_crc(fno.fname);
      if(is_damaged(crc))                 { bad++;     continue; }
      snprintf(title, sizeof(title), "%s", ft.title[0] ? ft.title : ft.set[0] ? ft.set : fno.fname);
      insert(title, fno.fname, crc, ft.board);
    }
    f_closedir(&dir);
  }
  sdc_unlock();
  debugf("Games: %u listed, %u without a valid footer, %u of an unknown board, %u damaged",
         list_n, old, foreign, bad);
}

unsigned games_count(void) { return list_n; }

bool games_item(unsigned i, games_item_t *out) {
  games_entry_t *e = list;
  while(e && i--) e = e->next;
  if(!e) return false;
  snprintf(out->title, sizeof(out->title), "%s", e->text);
  out->crc   = e->crc;
  out->board = e->board;
  return true;
}

void games_pick(const games_item_t *it) {
  taskENTER_CRITICAL();
  // a committed pick is on its way, a newer one comes too late
  if(!pick_committed) {
    pick.seq++;
    pick.crc   = it->crc;
    pick.board = it->board;
    memcpy(pick.title, it->title, sizeof(pick.title));
    pick_set = true;
  }
  taskEXIT_CRITICAL();
}

bool games_picked(games_pick_t *out) {
  bool set;
  taskENTER_CRITICAL();
  set = pick_set;
  if(set) *out = pick;
  taskEXIT_CRITICAL();
  return set;
}

bool games_pick_commit(unsigned seq) {
  bool fixed;
  taskENTER_CRITICAL();
  fixed = pick_set && pick.seq == seq;
  if(fixed) pick_committed = true;
  taskEXIT_CRITICAL();
  return fixed;
}

void games_pick_cancel(void) {
  taskENTER_CRITICAL();
  pick_set = pick_committed = false;
  taskEXIT_CRITICAL();
}

bool games_find(uint32_t crc, char *path, size_t n) {
  DIR dir;
  FILINFO fno;
  bool found = false;
  sdc_lock();
  if(f_opendir(&dir, CARD_MOUNTPOINT) == FR_OK) {
    while(!found && f_readdir(&dir, &fno) == FR_OK && fno.fname[0])
      if(!(fno.fattrib & AM_DIR) && games_name_crc(fno.fname) == crc) {
        int len = snprintf(path, n, "%s/%s", CARD_MOUNTPOINT, fno.fname);
        found = len > 0 && (size_t)len < n;
      }
    f_closedir(&dir);
  }
  sdc_unlock();
  return found;
}

bool games_footer_of(const char *path, games_footer_t *out) {
  FIL f;
  bool ok;
  sdc_lock();
  ok = f_open(&f, path, FA_READ) == FR_OK;
  if(ok) {
    ok = games_footer_read(&f, out);
    f_close(&f);
  }
  sdc_unlock();
  return ok;
}

void games_save_start(void) {
  const char *cwd = sdc_get_cwd(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  const char *img = sdc_get_image_name(MAX_DRIVES + RA_PATCH_ROM_IMAGE);
  FIL f;
  FRESULT r;
  sdc_lock();
  // the restart finds a start game of another board in the root only
  if(!cwd || !img || strcmp(cwd, CARD_MOUNTPOINT)) {
    r = f_unlink(GAMES_START_FILE);
    if(r == FR_OK) debugf("Games: no ROM in the card's root, the start game is removed");
  } else if((r = f_open(&f, GAMES_START_FILE, FA_WRITE | FA_CREATE_ALWAYS)) == FR_OK) {
    f_puts("; game20k: the game the Pico starts at power-on, written by Save settings\nstart=", &f);
    f_puts(img, &f);
    f_puts("\n", &f);
    bool ok = f_error(&f) == 0;
    if(f_close(&f) != FR_OK) ok = false;
    debugf("Games: start game %s %s", img, ok ? "saved" : "not saved");
  } else
    debugf("Games: %s not written (error %d)", GAMES_START_FILE, (int)r);
  sdc_unlock();
}

bool games_start_file(char *name, size_t n, games_footer_t *ft) {
  char line[FF_LFN_BUF + 16], path[GAMES_PATH_MAX];
  FIL f;
  bool found = false;
  sdc_lock();
  if(f_open(&f, GAMES_START_FILE, FA_READ) == FR_OK) {
    while(!found && f_gets(line, sizeof(line), &f)) {
      size_t len = strlen(line);
      while(len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
      if(strncasecmp(line, "start=", 6) || !line[6] || strchr(line + 6, '/')) continue;
      int w = snprintf(name, n, "%s", line + 6);
      found = w > 0 && (size_t)w < n;
    }
    f_close(&f);
  }
  sdc_unlock();
  if(!found) return false;
  snprintf(path, sizeof(path), "%s/%s", CARD_MOUNTPOINT, name);
  if(!games_footer_of(path, ft) || !menus_by_board(ft->board, NULL)) {
    debugf("Games: start game %s is missing, old or of an unknown board", name);
    return false;
  }
  return true;
}
