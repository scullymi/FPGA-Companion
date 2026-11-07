/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file game20k.h
 *  @brief What the files of FPGA Companion call of game20k.
 *
 *  With GAME20K defined (game20k.cmake) these are game20k's headers. Without it each call does
 *  nothing and the firmware is the one of FPGA Companion: the other boards, and the builds of
 *  bl616 and esp32, which never define it. Code that is more than a call is guarded by
 *  GAME20K in those files. */
#ifndef GAME20K_H
#define GAME20K_H

#ifdef GAME20K
#include "core_boot.h"
#include "game.h"
#include "game_switch.h"
#include "games.h"
#include "menu_pages.h"
#include "ra.h"
#include "ram_mirror.h"
#else
#include <stdbool.h>
#include <stddef.h>

#define GAME_ROM_IMAGE 0 /**< the image index of the ROM set, see game.h */

/* main.c */
static inline void marker_take(void) {}
static inline void start_game(void) {}
static inline bool ring_hops(unsigned char *probe) {
  (void)probe;
  return false;
}
static inline unsigned char ram_mirror_probe(void) { return 0; }
static inline void core_config_file(void) {}
static inline void core_images_open(void) {}
static inline void switch_rom(void) {}
static inline bool ra_patch_init(void) { return true; }
static inline void ra_mac_init(void) {}

/* sdc.c */
static inline void game_rom_start(const char *name) { (void)name; }
static inline void game_rom_data(const void *data, unsigned len) {
  (void)data;
  (void)len;
}
static inline void game_rom_gone(void) {}

/* ftpd.c, sysctrl.c, mcu_hw.c */
static inline void games_changed(const char *name) { (void)name; }
static inline void games_save_start(void) {}
static inline bool ra_task_hardcore(void) { return false; }
static inline void ra_task_core_value(char id, int value) {
  (void)id;
  (void)value;
}
static inline void ra_task_clock_set(void) {}

/* menu.c */
static inline const char *game_title(void) { return NULL; }
static inline bool menu_rom_pick(int drive, const char *name) {
  (void)drive;
  (void)name;
  return false;
}
#endif

#endif /* GAME20K_H */
