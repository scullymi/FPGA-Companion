/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi, parts from main.c of FPGA-Companion by Till Harbaum */
/** @file core_boot.c
 *  @brief Setting up the core that runs: its menu, its images, the ready action. */
#include <stdio.h>

#include "../mcu_hw.h"
#include "../config.h"
#include "../debug.h"
#include "../hid.h"
#include "../menu.h"
#include "../osd.h"
#include "../sdc.h"
#include "../sysctrl.h"
#include "../xml.h"
#include "banner.h"
#include "core_boot.h"
#include "game_switch.h"
#include "menus.h"               /* the menus of game20k's cores */
#include "ra_patch.h"
#include "ra_task.h"
#include "ram_mirror.h"

/* game20k: the menu of the core that runs, parsed into cfg, at the start and after
   a core switch (core_up()). Returns true for a game20k core whose menu this
   firmware lacks: the menu says so once it runs. */
bool core_config(unsigned char probe) {
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
void core_start(void) {
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
