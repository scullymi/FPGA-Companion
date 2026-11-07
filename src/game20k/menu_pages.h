/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file menu_pages.h
 *  @brief game20k's pages of the menu, see menu_pages.c, and what they need of menu.c. */
#ifndef MENU_PAGES_H
#define MENU_PAGES_H

#include <stdbool.h>

/* of menu.c */
int menu_page_selected(void);                  /**< @brief The line selected on the page, counted from 1. */
int menu_page_scroll(void);                    /**< @brief The first line in view, counted from 0. */
void menu_page_move(int selected, int scroll); /**< @brief Sets both. */
int menu_page_y(int row);  /**< @brief The baseline of a row of the display, counted from 0. */
int menu_page_x(void);     /**< @brief Where the text starts behind a mark. */
void menu_page_font(void); /**< @brief Sets the menu's font. */
extern const unsigned char icn_off_bits[]; /**< @brief The ring, 8 by 8 pixels. */
bool games_page_shown(void);               /**< @brief True when the Games page is on top. */
void menu_games_show(void);                /**< @brief Brings the Games page on screen without a key. */

/* of menu_pages.c */
#if GAME20K_RA
int ra_list_length(void);      /**< @brief The achievement list: its lines. */
int ra_list_first(void);       /**< @brief The achievement list: the line to open it on. */
void ra_list_draw(void);       /**< @brief The achievement list: draws it. */
void ra_list_select(int line); /**< @brief The achievement list: the details of a line. */
void menu_ra_status(void);     /**< @brief The Account dialog. */
#endif
int games_list_length(void);      /**< @brief The Games page: its lines. */
void games_list_draw(void);       /**< @brief The Games page: draws it. */
void games_list_select(int line); /**< @brief The Games page: starts the game of a line. */
int games_list_scroll(int sel);   /**< @brief The scroll that shows line sel. */
int games_list_first(void);       /**< @brief Reads the card, the line to open the page on. */
bool games_list_reread(void);     /**< @brief Reads the changed card, true when the selection moved. */
void menu_version_status(void);   /**< @brief The Version dialog. */
void menu_dips_set(void);         /**< @brief Sets the DIP switches as the game's set expects. */
bool menu_rom_pick(int drive,
                   const char *name); /**< @brief True when a ROM file picked goes no further here. */
bool menu_page_event(long cmd);       /**< @brief The events of the pages, true when cmd is one of them. */

#endif /* MENU_PAGES_H */
