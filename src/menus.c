/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file menus.c
 *  @brief The lookup in the menu table and the pick of the menu at boot, see menus.h.
 *
 *  No task state and no I/O: the file links into a host test. */
#include <string.h>

#include "menus.h"

const menus_entry_t *menus_by_board(unsigned char board, const char *set) {
  const menus_entry_t *any = NULL;
  unsigned i;
  for(i = 0; i < menus_rows_n; i++) {
    const menus_entry_t *e = &menus_rows[i];
    if(e->board != board) continue;
    // a set's own menu goes first, the board's is the fallback
    if(set && e->set && !strcmp(e->set, set)) return e;
    if(!e->set && !any) any = e;
  }
  return any;
}

menus_why_t menus_pick(unsigned char board, uint16_t tag, const char *set, const menus_entry_t **menu) {
  const menus_entry_t *e = board ? menus_by_board(board, set) : NULL;
  *menu = NULL;
  if(!board)        return MENUS_NO_HEADER;
  if(!e)            return MENUS_NO_BOARD;
  if(e->tag != tag) return MENUS_MISMATCH;
  *menu = e;
  return MENUS_FOUND;
}
