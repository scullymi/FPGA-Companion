/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_text.c
 *  @brief Titles and descriptions of a set's achievements in one block.
 *
 *  ra_patch keeps them for the menu and the banner in a block sized to the set. It fills
 *  the block from the parsed set, or, when the heap has no room for it beside the parsed
 *  set, from the set's text once the parsed set, which holds these very strings, is
 *  freed. Both ways cut alike. */
#include <string.h>

#include "rc_util.h"
#include "rcheevos/src/rapi/rc_api_common.h"
#include "ra_patch.h"
#include "ra_text.h"

size_t ra_text_size(const char *title, const char *desc) {
  return strnlen(title ? title : "", RA_PATCH_TITLE_MAX - 1) + 1 +
         strnlen(desc ? desc : "", RA_PATCH_DESC_MAX - 1) + 1;
}

/* Copies s, cut to max - 1 characters, with its NUL to at. Returns the byte after it. */
static char *put(char *at, const char *s, size_t max, const char **out) {
  size_t n = strnlen(s ? s : "", max - 1);
  memcpy(at, s ? s : "", n);
  at[n] = 0;
  *out = at;
  return at + n + 1;
}

bool ra_text_put(char **at, const char *end, const char *title, const char *desc,
                 const char **t, const char **d) {
  if(!*at || (size_t)(end - *at) < ra_text_size(title, desc)) return false;
  *at = put(*at, title, RA_PATCH_TITLE_MAX, t);
  *at = put(*at, desc, RA_PATCH_DESC_MAX, d);
  return true;
}

bool ra_text_each(const char *json, size_t len, ra_text_fn fn, void *arg) {
  rc_json_field_t top[] = { RC_JSON_NEW_FIELD("PatchData") };
  rc_json_field_t pd[]  = { RC_JSON_NEW_FIELD("Achievements") };
  rc_json_field_t af[]  = { RC_JSON_NEW_FIELD("ID"), RC_JSON_NEW_FIELD("Title"),
                            RC_JSON_NEW_FIELD("Description") };
  rc_json_iterator_t it;
  rc_json_field_t arr;
  uint32_t num, id;

  // the list in PatchData, found as activate_deferred() in ra_patch.c finds it
  it.json = json; it.end = json + len;
  if(!rc_json_get_array_entry_object(top, 1, &it) || !top[0].value_start) return false;
  it.json = top[0].value_start; it.end = top[0].value_end;
  if(!rc_json_get_array_entry_object(pd, 1, &it) ||
     !rc_json_get_optional_array(&num, &arr, &pd[0], "Achievements")) return false;
  it.json = arr.value_start; it.end = arr.value_end;
  while(rc_json_get_array_entry_object(af, 3, &it)) {
    rc_buffer_t buf;
    const char *title = NULL, *desc = NULL;
    if(!rc_json_get_unum(&id, &af[0], "ID")) continue;
    rc_buffer_init(&buf);
    // a missing string counts as none, one that finds no memory ends the walk
    bool ok = (rc_json_get_string(&title, &buf, &af[1], "Title") || !af[1].value_start) &&
              (rc_json_get_string(&desc, &buf, &af[2], "Description") || !af[2].value_start);
    if(ok) fn(arg, id, title, desc);
    rc_buffer_destroy(&buf);
    if(!ok) return false;
  }
  return true;
}
