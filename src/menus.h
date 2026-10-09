/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file menus.h
 *  @brief The menus of game20k's cores, built into the firmware, and the pick of one at boot.
 *
 *  A game20k core carries no menu. It names its board and its interface tag in the RAM
 *  mirror header (bytes 12 and 16 to 19), and the firmware takes the board's menu from its
 *  table when the tag there is the same: both come from the same menu sources. The table
 *  is a file of its own that CMake takes from GAME20K_MENU_TABLE. game20k's
 *  scripts/build_companion.sh passes the one its scripts/make_fw_tables.py generates, a
 *  build without it links menus_example.c, which holds no menu of a core. */
#ifndef MENUS_H
#define MENUS_H

#include <stddef.h>
#include <stdint.h>

/** @brief The menu of a board, or of one set on it. */
typedef struct {
  unsigned char board;   /**< header byte 12 of the core */
  const char   *set;     /**< NULL for the board's menu, else the set this menu is for, ahead of the board's */
  uint16_t      tag;     /**< IFACE_TAG of the core built from the same sources, header bytes 16 and 17 */
  const char   *xml;     /**< the menu, as the Companion parses it */
} menus_entry_t;

extern const menus_entry_t menus_rows[];
extern const unsigned      menus_rows_n;      /**< rows in menus_rows */
extern const char          menus_basic_xml[]; /**< the menu for a core the table has no fitting menu for */
extern const char          menus_origin[];    /**< "generated" for game20k's table, "example" for menus_example.c */

/** @brief Why menus_pick() found a menu or none. */
typedef enum {
  MENUS_FOUND,      /**< the table's menu for this core */
  MENUS_NO_HEADER,  /**< no RAM mirror header of this layout: another core, it may bring its own menu */
  MENUS_NO_BOARD,   /**< a game20k core whose board the table lacks */
  MENUS_MISMATCH    /**< the board's menu has another tag: core and firmware of different sources */
} menus_why_t;

/** @brief The entry of the set on that board, else the board's, NULL when there is none. set may be NULL. */
const menus_entry_t *menus_by_board(unsigned char board, const char *set);

/** @brief Picks the menu for the core: board 0 means no valid header was read.
 *
 *  *menu gets the entry for MENUS_FOUND and NULL otherwise. */
menus_why_t menus_pick(unsigned char board, uint16_t tag, const char *set, const menus_entry_t **menu);

#endif /* MENUS_H */
