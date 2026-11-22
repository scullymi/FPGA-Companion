/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file games.c
 *  @brief The Games page's list, the game picked there and the start game.
 *
 *  The list is every *.rom in the card's root and in the roms folder of a USB drive whose
 *  footer is valid (games_file.c) and names a board with a menu in this firmware (menus.h),
 *  sorted by title. A name in both places is listed once, the card's file wins. It is read
 *  in the menu task when the page opens and when the card changed, one footer per
 *  file, and holds only titles and names. The content is checked when it streams
 *  to the core: sdc.c compares the SHA-256 the stream yields with the footer's, a
 *  file that fails stays off the list until the card changes. With JTAG, each line also
 *  says where its board's core would come from: a file in the drive's cores folder
 *  (games_usb_core()), else the flash ring.
 *
 *  A game picked on the page reaches game_switch.c as the CRC-32 of its file name and its
 *  board: switch_step() switches the core for another board, and switch_rom()
 *  finds the file again by that CRC. The start game is a file's path in
 *  GAMES_START_FILE: config.ini belongs to the user and holds secrets, the ini of a core
 *  is read only by that core, so the start game has a file of its own that every core
 *  reads at power-on.
 *
 *  A USB drive mounts a few seconds after the start. A start game, a game picked or an
 *  image of the ini on the drive waits for it, see games_usb_wait(). */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <FreeRTOS.h>
#include <task.h>
#include <ff.h>
#include "../debug.h"
#include "../mcu_hw.h" // ENABLE_JTAG, mcu_hw_usb_msc_present()
#include "ram_mirror.h"
#include "stone.h"
#include "../sdc.h"
#include "menus.h"   // the boards this firmware has a core menu for
#include "game.h"    // GAME_ROM_IMAGE
#include "../menu.h" // menu_notify()
#include "games.h"

#define GAMES_DAMAGED_MAX 4    /**< damaged files remembered, the oldest gives way */
#define GAMES_USB_WAIT_MS 5000 /**< from the start: a drive at a hub mounts well within this at power-on */
#define GAMES_USB_POLL_MS 50   /**< between two tries to mount it */

/* One line of the list: the title, then the file name, in one allocation. */
typedef struct games_entry {
  struct games_entry *next;
  uint32_t crc; // of the file name
  unsigned char board;
  games_core_t core;
  const char *name; // behind the title in text[]
  char text[];      // title NUL name NUL
} games_entry_t;

/* where the ROM files are, in the order a name is looked up */
static const char *const games_dirs[] = {CARD_MOUNTPOINT, "/usb/roms"};
#define GAMES_DIRS (sizeof(games_dirs) / sizeof(games_dirs[0]))

static games_entry_t *list; // menu task only
static unsigned list_n;
static volatile bool stale = true;          // the card changed, any task sets it
static uint32_t damaged[GAMES_DAMAGED_MAX]; // CRCs of name, 0 for none, under a critical section
static unsigned damaged_next;
static games_pick_t pick; // under a critical section
static bool pick_set, pick_committed;
static FATFS usb_fs;             // the USB drive's file system, under sdc_lock
static bool usb_mounted;         // under sdc_lock
static volatile bool usb_missed; // a wait for the drive ran out, until games_usb_missed()

uint32_t games_name_crc(const char *name) { return games_crc32(name, strlen(name)); }

bool games_stale(void) { return stale; }

/* True for a path on the USB drive, compared as sdc.c and games_in_dir() do, without case. */
static bool on_usb(const char *path) {
  return path && !strncasecmp(path, "/usb", 4) && (!path[4] || path[4] == '/');
}

bool games_usb_mount(void) {
  bool ok;
  sdc_lock();
  ok = usb_mounted;
  // the USB stack has the drive ready, its file system is mounted once
  if(!ok && mcu_hw_usb_msc_present()) ok = usb_mounted = f_mount(&usb_fs, "/usb", 1) == FR_OK;
  sdc_unlock();
  return ok;
}

bool games_usb_unmount(void) {
  sdc_lock();
  usb_mounted = false;
  bool ok     = f_unmount("/usb") == FR_OK;
  sdc_unlock();
  return ok;
}

bool games_usb_wait(void) {
  // later than this after the start, a drive not mounted is not there: only a wait that
  // began before and ran out is a drive that did not come in time
  bool early = xTaskGetTickCount() < pdMS_TO_TICKS(GAMES_USB_WAIT_MS), waited = false;
  while(!games_usb_mount()) {
    if(xTaskGetTickCount() >= pdMS_TO_TICKS(GAMES_USB_WAIT_MS)) {
      if(early) {
        debugf("USB: no drive mounted within %u ms of the start", GAMES_USB_WAIT_MS);
        usb_missed = true;
      }
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(GAMES_USB_POLL_MS));
    waited = true;
  }
  if(waited)
    debugf("USB: drive mounted %lu ms after the start",
           (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
  return true;
}

bool games_usb_missed(void) {
  taskENTER_CRITICAL();
  bool missed = usb_missed;
  usb_missed  = false;
  taskEXIT_CRITICAL();
  return missed;
}

void games_usb_defaults(void) {
  // the images of the ini, Save settings writes a drive's path for a file from there
  for(int d = 0; d < MAX_DRIVES + MAX_IMAGES; d++) {
    if(sdc_get_image_name(d) && on_usb(sdc_get_cwd(d))) {
      games_usb_wait();
      break;
    }
  }
  if(games_usb_missed()) menu_notify(MENU_EVENT_USB_MISSING);
}

void games_changed(const char *name) {
  // only the root's ROM files are listed, but a file moved there counts as well
  if(name && *name) {
    size_t len = strlen(name);
    if(len < 4 || strcasecmp(name + len - 4, ".rom")) return;
  }
  taskENTER_CRITICAL();
  memset(damaged, 0, sizeof(damaged));
  bool was = stale;
  stale    = true;
  taskEXIT_CRITICAL();
  if(!was) menu_notify(MENU_EVENT_GAMES_CHANGED); // one is enough until the next read
}

void games_damaged(const char *name) {
  uint32_t crc = games_name_crc(name);
  taskENTER_CRITICAL();
  damaged[damaged_next] = crc;
  damaged_next          = (damaged_next + 1) % GAMES_DAMAGED_MAX;
  bool was              = stale;
  stale                 = true;
  taskEXIT_CRITICAL();
  if(!was) menu_notify(MENU_EVENT_GAMES_CHANGED);
}

static bool is_damaged(uint32_t crc) {
  bool hit = false;
  taskENTER_CRITICAL();
  for(unsigned i = 0; i < GAMES_DAMAGED_MAX; i++)
    if(damaged[i] && damaged[i] == crc) hit = true;
  taskEXIT_CRITICAL();
  return hit;
}

bool games_in_dir(const char *dir) {
  for(unsigned d = 0; d < GAMES_DIRS; d++)
    if(dir && !strcasecmp(dir, games_dirs[d])) return true;
  return false;
}

/* True when a file of that name is in the list already. */
static bool listed(uint32_t crc) {
  for(games_entry_t *e = list; e; e = e->next)
    if(e->crc == crc) return true;
  return false;
}

#ifdef ENABLE_JTAG
bool games_usb_core(unsigned char board, char *path, size_t n) {
  const menus_entry_t *e = menus_by_board(board, NULL);
  FILINFO fno;
  if(!mcu_hw_usb_msc_present() || !e || !e->core) return false;
  int len = snprintf(path, n, "/usb/cores/%s.bin", e->core);
  if(len < 0 || (size_t)len >= n) return false;
  return f_stat(path, &fno) == FR_OK;
}
#endif

games_core_t games_core_of(unsigned char board) {
#ifdef ENABLE_JTAG
  char path[GAMES_PATH_MAX];
  if(games_usb_core(board, path, sizeof(path))) return GAMES_CORE_USB;
#endif
  if(ram_mirror_slots_known && stone_slot(ram_mirror_slots, true, board) == STONE_NONE)
    return GAMES_CORE_NONE;
  return GAMES_CORE_FLASH;
}

/* Where the core of board comes from, caller holds sdc_lock. A board in the list already
   has its answer there, so the drive is asked once per board and read. */
static games_core_t core_of(unsigned char board) {
  for(games_entry_t *e = list; e; e = e->next)
    if(e->board == board) return e->core;
  return games_core_of(board);
}

/* Puts one file into the list at its place by title. */
static void insert(const char *title, const char *name, uint32_t crc, unsigned char board) {
  games_core_t core = core_of(board);
  size_t tl = strlen(title), nl = strlen(name);
  games_entry_t *e = pvPortMalloc(sizeof(*e) + tl + nl + 2), **at = &list;
  if(!e) return;
  memcpy(e->text, title, tl + 1);
  memcpy(e->text + tl + 1, name, nl + 1);
  e->name  = e->text + tl + 1;
  e->crc   = crc;
  e->board = board;
  e->core  = core;
  while(*at && games_order((*at)->text, (*at)->name, title, name) <= 0) at = &(*at)->next;
  e->next = *at;
  *at     = e;
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
  stale  = false; // before the read: a change while it runs reads again
  sdc_lock();
  // a folder that is not there, e.g. without a USB drive, adds nothing
  for(unsigned d = 0; d < GAMES_DIRS; d++) {
    if(f_opendir(&dir, games_dirs[d]) != FR_OK) continue;
    while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
      size_t len = strlen(fno.fname);
      if((fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) || len < 5 || strcasecmp(fno.fname + len - 4, ".rom"))
        continue;
      snprintf(path, sizeof(path), "%s/%s", games_dirs[d], fno.fname);
      bool ok = f_open(&f, path, FA_READ) == FR_OK;
      if(ok) {
        ok = games_footer_read(&f, &ft);
        f_close(&f);
      }
      // a file without a valid footer is an old one (or none at all), a board
      // without a menu here has no core in this firmware's ring
      if(!ok) {
        old++;
        continue;
      }
      if(!menus_by_board(ft.board, NULL)) {
        foreign++;
        continue;
      }
      uint32_t crc = games_name_crc(fno.fname);
      if(is_damaged(crc)) {
        bad++;
        continue;
      }
      if(listed(crc)) continue; /* the same name in an earlier folder */
      snprintf(title, sizeof(title), "%s", ft.title[0] ? ft.title : ft.set[0] ? ft.set : fno.fname);
      insert(title, fno.fname, crc, ft.board);
    }
    f_closedir(&dir);
  }
  sdc_unlock();
  debugf("Games: %u listed, %u without a valid footer, %u of an unknown board, %u damaged", list_n, old,
         foreign, bad);
}

unsigned games_count(void) { return list_n; }

bool games_item(unsigned i, games_item_t *out) {
  games_entry_t *e = list;
  while(e && i--) e = e->next;
  if(!e) return false;
  snprintf(out->title, sizeof(out->title), "%s", e->text);
  out->crc   = e->crc;
  out->board = e->board;
  out->core  = e->core;
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
  for(unsigned d = 0; !found && d < GAMES_DIRS; d++) {
    // not on the card: the drive may still mount, e.g. after the Pico restarted for a core switch
    if(on_usb(games_dirs[d]) && !games_usb_wait()) continue;
    sdc_lock();
    if(f_opendir(&dir, games_dirs[d]) == FR_OK) {
      while(!found && f_readdir(&dir, &fno) == FR_OK && fno.fname[0])
        if(!(fno.fattrib & AM_DIR) && games_name_crc(fno.fname) == crc) {
          int len = snprintf(path, n, "%s/%s", games_dirs[d], fno.fname);
          found   = len > 0 && (size_t)len < n;
        }
      f_closedir(&dir);
    }
    sdc_unlock();
  }
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
  const char *cwd = sdc_get_cwd(MAX_DRIVES + GAME_ROM_IMAGE);
  const char *img = sdc_get_image_name(MAX_DRIVES + GAME_ROM_IMAGE);
  FIL f;
  FRESULT r;
  sdc_lock();
  // power-on finds a start game of another board in the folders of the list only
  if(!cwd || !img || !games_in_dir(cwd)) {
    r = f_unlink(GAMES_START_FILE);
    if(r == FR_OK) debugf("Games: no ROM in a folder of the list, the start game is removed");
  } else if((r = f_open(&f, GAMES_START_FILE, FA_WRITE | FA_CREATE_ALWAYS)) == FR_OK) {
    f_puts("; game20k: the game started at power-on, written by Save settings\nstart=", &f);
    f_puts(cwd, &f);
    f_puts("/", &f);
    f_puts(img, &f);
    f_puts("\n", &f);
    bool ok = f_error(&f) == 0;
    if(f_close(&f) != FR_OK) ok = false;
    debugf("Games: start game %s %s", img, ok ? "saved" : "not saved");
  } else debugf("Games: %s not written (error %d)", GAMES_START_FILE, (int)r);
  sdc_unlock();
}

bool games_start_file(char *name, size_t n, games_footer_t *ft) {
  char line[GAMES_PATH_MAX + 8], path[GAMES_PATH_MAX];
  FIL f;
  bool found = false;
  sdc_lock();
  if(f_open(&f, GAMES_START_FILE, FA_READ) == FR_OK) {
    while(!found && f_gets(line, sizeof(line), &f)) {
      size_t len = strlen(line);
      while(len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
      if(strncasecmp(line, "start=", 6)) continue;
      // a file in a folder of the list, the one games_find() looks in, and a bare name is none
      char *slash = strrchr(line + 6, '/');
      if(!slash || !slash[1]) continue;
      *slash = 0;
      if(!games_in_dir(line + 6)) continue;
      int wp = snprintf(path, sizeof(path), "%s/%s", line + 6, slash + 1);
      int wn = snprintf(name, n, "%s", slash + 1);
      found  = wp > 0 && (size_t)wp < sizeof(path) && wn > 0 && (size_t)wn < n;
    }
    f_close(&f);
  }
  sdc_unlock();
  if(!found) return false;
  if(on_usb(path) && !games_usb_wait()) {
    debugf("Games: start game %s is on a USB drive that did not come", path);
    return false;
  }
  // the file the switch finds by the name's CRC, the card's when both have that name, gives
  // the board: the core switched to is the one of the ROM that streams
  if(!games_find(games_name_crc(name), path, sizeof(path)) || !games_footer_of(path, ft) ||
     !menus_by_board(ft->board, NULL)) {
    debugf("Games: start game %s is missing, old or of an unknown board", path);
    return false;
  }
#ifdef ENABLE_JTAG
  // only a drive has its core: the core switch at power-on loads it from there
  if(ram_mirror_slots_known && stone_slot(ram_mirror_slots, true, ft->board) == STONE_NONE) games_usb_wait();
#endif
  return true;
}
