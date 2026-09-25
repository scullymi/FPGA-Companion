/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/* The achievement set of the current game.

   The conditions belong to RetroAchievements and are not compiled into the
   firmware. The set lies on the card as RA_PATCH_FILE, the server's reply to
   r=patch, and rcheevos' own parser reads it (rc_api_runtime.c). Nothing here
   interprets the JSON. Titles are copied into a small table, the parsed
   conditions live inside rcheevos. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "rc_api_runtime.h"
#include <ff.h>
#include "debug.h"
#include "sdc.h"
#include "ra_patch.h"

#define RA_PATCH_BODY_MAX   40960        /* Galaga on 24.09.2026: 15757 bytes */
#define RA_PATCH_MAX        64           /* achievements kept, Galaga has 17 */
#define RA_PATCH_TITLE_MAX  32           /* the banner shows 24 characters */
#define RA_PATCH_FILE       "/sd/ra_patch.json"
/* "Warning: Unknown Emulator", which the server adds for clients it does not
   know. Not a Galaga achievement. */
#define RA_PATCH_WARNING_ID 101000001u

static char     body[RA_PATCH_BODY_MAX];
static unsigned body_len;
static struct { unsigned id; char title[RA_PATCH_TITLE_MAX]; } set[RA_PATCH_MAX];
static unsigned set_n;

unsigned ra_patch_count(void) { return set_n; }

unsigned ra_patch_index(unsigned id) {
  unsigned i;
  for(i = 0; i < set_n; i++) if(set[i].id == id) return i + 1;
  return 0;
}

const char *ra_patch_title(unsigned id) {
  unsigned i = ra_patch_index(id);
  return i ? set[i - 1].title : "";
}

static bool card_read(void) {
  FIL f;
  UINT got = 0;
  bool ok = false;
  sdc_lock();
  if(f_open(&f, RA_PATCH_FILE, FA_READ) == FR_OK) {
    if(f_read(&f, body, sizeof(body) - 1, &got) == FR_OK && got > 0) {
      body[got] = 0;
      body_len = got;
      ok = true;
    }
    f_close(&f);
  }
  sdc_unlock();
  return ok;
}

static bool core_item(const rc_api_achievement_definition_t *a) {
  return a->category == RC_ACHIEVEMENT_CATEGORY_CORE && a->id != RA_PATCH_WARNING_ID;
}

/* Lets rcheevos parse the body, prints why when it is unusable. The caller
   destroys r in every case, as rc_api asks. */
static int parse_body(rc_api_fetch_game_data_response_t *r) {
  rc_api_server_response_t sr;
  memset(&sr, 0, sizeof(sr));
  sr.body = body;
  sr.body_length = body_len;
  sr.http_status_code = 200;
  if(rc_api_process_fetch_game_data_server_response(r, &sr) != RC_OK || !r->response.succeeded) {
    debugf("RA: set unusable: %s",
           r->response.error_message ? r->response.error_message : "no valid JSON");
    return -1;
  }
  if(r->id != RA_PATCH_GAME_ID) {
    debugf("RA: set is for game %u, not %u", (unsigned)r->id, (unsigned)RA_PATCH_GAME_ID);
    return -1;
  }
  return 0;
}

/* Activates the core achievements and fills the title table. A condition that
   rcheevos rejects is reported, it would otherwise never fire in silence. */
static int activate_set(rc_runtime_t *rt, const rc_api_fetch_game_data_response_t *r) {
  unsigned i, rejected = 0;

  set_n = 0;
  for(i = 0; i < r->num_achievements; i++) {
    const rc_api_achievement_definition_t *a = &r->achievements[i];
    if(!core_item(a)) continue;
    if(set_n >= RA_PATCH_MAX) {
      debugf("RA: more than %u achievements, the rest is ignored", (unsigned)RA_PATCH_MAX);
      break;
    }
    int rv = rc_runtime_activate_achievement(rt, a->id, a->definition, NULL, 0);
    if(rv != RC_OK) {
      debugf("RA: condition %u (%s) rejected, code %d, it will never fire",
             (unsigned)a->id, a->title ? a->title : "", rv);
      rejected++;
      continue;
    }
    set[set_n].id = a->id;
    snprintf(set[set_n].title, sizeof(set[set_n].title), "%s", a->title ? a->title : "");
    set_n++;
  }

  debugf("RA: set for '%s': %u achievements active, %u rejected, %u leaderboards ignored",
         r->title ? r->title : "?", set_n, rejected, (unsigned)r->num_leaderboards);
  return (int)set_n;
}

int ra_patch_load(rc_runtime_t *rt) {
  rc_api_fetch_game_data_response_t r;
  int n = -1;

  if(!card_read()) {
    debugf("RA: no set on the card (%s), no achievements", RA_PATCH_FILE);
    return -1;
  }
  memset(&r, 0, sizeof(r));
  if(parse_body(&r) == 0) n = activate_set(rt, &r);
  rc_api_destroy_fetch_game_data_response(&r);
  return n;
}
