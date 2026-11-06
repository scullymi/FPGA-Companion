/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file core_boot.c
 *  @brief game20k's part in setting up a core: the menu of its board, what follows its images, a new core. */
#include <stdio.h>

#include "../mcu_hw.h"
#include "../config.h"
#include "../debug.h"
#include "../hid.h"
#include "../menu.h"
#include "../osd.h"
#include "../sdc.h"
#include "../sysctrl.h"
#include "banner.h"
#include "core_boot.h"
#include "menus.h"               /* the menus of game20k's cores */
#include "ra_patch.h"
#include "ra_task.h"
#include "ram_mirror.h"

/* A menu of one's own in config.xml can set the DIP switches without a reset, and it is
   what decides the mode: no hardcore with it (the core also takes DIP switches only
   while in reset). */
void core_config_file(void) {
  ra_task_hardcore_block(RA_HC_BLOCK_XML, true);
}

/* No config.xml on the card: the firmware's menu for the core's board when the interface
   tags agree. Else a core without a game20k header may bring its own menu (READ_CFG),
   and the last resort is the basic menu: there is always an OSD. A game20k core of
   another release gets the basic menu with a message, and without hardcore when it has
   the board but another tag: its DIP switches may mean something else than this
   firmware's menu says. *mismatch tells the menu to say so. The text is only read, and
   it must not be freed: the built-in menus lie in the flash. */
char *core_menu(unsigned char probe, bool *mismatch) {
  const menus_entry_t *m;
  menus_why_t why = menus_pick(ram_mirror_board, ram_mirror_tag, NULL, &m);
  char *c = NULL;
  if(m) {
    debugf("Loading the built-in XML config of board %u", ram_mirror_board);
    c = (char *)m->xml;
  } else if(why == MENUS_NO_HEADER && (c = sys_get_config()) != NULL)
    debugf("Loading XML config from core");
  else {
    debugf("No menu for this core (board %u, tag %04x, why %d), loading the basic XML config",
           ram_mirror_board, ram_mirror_tag, why);
    c = (char *)menus_basic_xml;
  }
  *mismatch = why != MENUS_FOUND && probe != 0xE1;
  ra_task_hardcore_block(RA_HC_BLOCK_MENU, why == MENUS_MISMATCH);   // the core before may have set it
  return c;
}

/* After the images are open. No ROM in the core, its file is missing, old or rejected:
   the Games page opens instead of a dark screen with a closed menu. No image0 in the
   ini, or the core rejected it (the transfer is closed then): the game is decided now,
   before the ready action, from the board alone. With a stream running, its last chunk
   settles. */
void core_images_open(void) {
  if(!sdc_get_image_name(MAX_DRIVES + RA_PATCH_ROM_IMAGE)) menu_notify(MENU_EVENT_GAMES);
  if(!sdc_image_upload_in_progress()) ra_patch_settle();
}

/* game20k: a new core is up, set it up as the start sets one up: the old core's
   images and game go, its menu is freed, the new one is parsed and built with the
   variables and the ini of the new core, and its game streams. WiFi, the login,
   the clock, FTP and telnet stay. The menu task is parked, probe is the verdict on
   the new core's header, t0 when the switch began. */
void core_setup(unsigned char probe, TickType_t t0) {
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
