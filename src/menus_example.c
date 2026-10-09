/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file menus_example.c
 *  @brief EXAMPLE, not the game20k table: a menu table without a menu of a core.
 *
 *  A build without GAME20K_MENU_TABLE links this file, so the fork builds on its own.
 *  Such a firmware has no menu for a game20k core: every one gets the basic menu
 *  below, and the Version dialog says "example". A core that brings its own menu
 *  still gets that one. game20k's scripts/build_companion.sh passes the table that
 *  scripts/make_fw_tables.py generates from its menu sources, one row per board:
 *
 *    static const char menu_galaga_hdmi[] = "<?xml version=\"1.0\" ...";
 *    const menus_entry_t menus_rows[] = {
 *      { 1, NULL, 0x1234u, menu_galaga_hdmi },
 *    };
 *
 *  and a basic menu with the settings every game20k core has. */
#include "menus.h"

const char menus_origin[] = "example";

// C has no empty array: one zero row gives the table a size, menus_rows_n keeps it
// out of every lookup
const menus_entry_t menus_rows[1];
const unsigned      menus_rows_n = 0;

// the least a menu needs: the core out of reset, a reset, and the Status page
const char menus_basic_xml[] =
  "<config name=\"example\">\n"
  "  <actions>\n"
  "    <action name=\"netinfo\"/>\n"
  "    <action name=\"verinfo\"/>\n"
  "    <action name=\"ready\"><set id=\"R\" value=\"0\"/></action>\n"
  "    <action name=\"reset\"><set id=\"R\" value=\"1\"/><delay ms=\"10\"/><set id=\"R\" value=\"0\"/></action>\n"
  "  </actions>\n"
  "  <menu label=\"Unknown core\">\n"
  "    <menu label=\"Status\">\n"
  "      <button label=\"Network\" action=\"netinfo\"/>\n"
  "      <button label=\"Version\" action=\"verinfo\"/>\n"
  "    </menu>\n"
  "    <button label=\"Reset\" action=\"reset\"/>\n"
  "  </menu>\n"
  "</config>\n";
