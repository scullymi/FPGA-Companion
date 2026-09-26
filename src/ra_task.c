/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_task.c
 *  @brief The RetroAchievements task: all network work for the achievements.
 *
 *  It runs apart from com_task, so the game loop never waits for the server. It
 *  sleeps until it is woken, for now only by the NTP clock. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <FreeRTOS.h>
#include <task.h>
#include "rc_api_runtime.h"

#include "debug.h"
#include "ra_net.h"
#include "ra_patch.h"
#include "ra_task.h"

#define RA_CLOCK_VALID  1735689600u   /* 2025-01-01, an earlier time() is not set yet */
#define RA_CLOCK_WAIT   60000u        /* ms without time from NTP before the log says so */
#define RA_BACKOFF_MIN  10000u        /* ms, first pause after a failed request */
#define RA_BACKOFF_MAX  600000u       /* ms, the pause doubles up to this */

static TaskHandle_t task;             // NULL until ra_task_start()
static char         reply_buf[512];   // replies to the small requests, a few hundred bytes

void ra_task_clock_set(void) {
  if(task) xTaskNotifyGive(task);     // the task checks time() itself when it wakes
}

/* Asks the server which game the game hash belongs to. That needs no account,
   so it proves TLS, the certificate check and the server before the token is
   ever sent. */
static bool check_server(void) {
  rc_api_resolve_hash_request_t params;
  rc_api_request_t request;
  char path[128];
  ra_reply_t reply;
  bool ok = false;

  // rcheevos builds the query (r=gameid&m=<hash>), it goes out as GET
  memset(&params, 0, sizeof(params));
  params.game_hash = RA_PATCH_GAME_HASH;
  if(rc_api_init_resolve_hash_request(&request, &params) != RC_OK) return false;
  snprintf(path, sizeof(path), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);

  if(ra_net_get(path, reply_buf, sizeof(reply_buf), &reply) != 0 ||
     reply.result != 0 || reply.status != 200)
    return false;

  // rcheevos also reads the JSON reply
  rc_api_resolve_hash_response_t response;
  rc_api_server_response_t server;
  memset(&server, 0, sizeof(server));
  server.body = reply_buf;
  server.body_length = reply.len;
  server.http_status_code = (int)reply.status;
  if(rc_api_process_resolve_hash_server_response(&response, &server) == RC_OK &&
     response.response.succeeded) {
    debugf("RA: server reached, hash %s is game %u", RA_PATCH_GAME_HASH, (unsigned)response.game_id);
    ok = true;
  }
  rc_api_destroy_resolve_hash_response(&response);
  return ok;
}

static void ra_task_main(__attribute__((unused)) void *p) {
  uint32_t backoff = RA_BACKOFF_MIN;

  debugf("RA: client %s", ra_user_agent());

  // 1. certificates are checked against the clock, so wait until NTP has set it.
  //    Without a time server this can take forever, so say it once in the log.
  TickType_t wait = pdMS_TO_TICKS(RA_CLOCK_WAIT);
  while((unsigned long)time(NULL) < RA_CLOCK_VALID) {
    if(!ulTaskNotifyTake(pdTRUE, wait)) {
      debugf("RA: no time from NTP yet, achievements wait for it (see [NTP] IP in config.ini)");
      wait = portMAX_DELAY;   // said once, now sleep until the clock is set
    }
  }

  // 2. reach the server, with growing pauses while it or the network is down
  while(!check_server()) {
    debugf("RA: server not reached, next try in %lu s", (unsigned long)(backoff / 1000));
    vTaskDelay(pdMS_TO_TICKS(backoff));
    backoff = backoff < RA_BACKOFF_MAX / 2 ? backoff * 2 : RA_BACKOFF_MAX;
  }

  // 3. nothing more to do yet, login and unlocks follow here
  for(;;) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void ra_task_start(void) {
  if(task) return;
  // below com_task, like wifi_task. 2048 words: about 5 KB were still free
  // after a TLS handshake (measured 26.09.2026)
  if(xTaskCreate(ra_task_main, "RA", 2048, NULL, configMAX_PRIORITIES - 10, &task) != pdPASS)
    debugf("RA: task could not be created");
}
