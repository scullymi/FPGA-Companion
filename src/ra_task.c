/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_task.c
 *  @brief The RetroAchievements task: all network work for the achievements.
 *
 *  It runs apart from com_task, so the game loop never waits for the server. It
 *  waits on one queue set for everything that can wake it: unlocks from com_task,
 *  the NTP clock and, inside ra_net_get(), the end of a request. So an unlock is
 *  written to the card at once, even while a request is running. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_error.h"

#include "debug.h"
#include "inifile.h"
#include "ra_net.h"
#include "ra_patch.h"
#include "ra_queue.h"
#include "ra_state.h"
#include "ra_task.h"

#define RA_CLOCK_WAIT   60000u        /**< ms without time from NTP before the log says so */
#define RA_BACKOFF_MIN  10000u        /**< ms, first pause after a failed request */
#define RA_BACKOFF_MAX  600000u       /**< ms, the pause doubles up to this */
#define RA_EVENTS       (RA_QUEUE_HANDOVER + 2)   /**< queue set: the unlocks, the clock, the end of a request */
#define RA_TASK_STACK   2048          /**< words, about 5 KB stayed free after a TLS handshake (26.09.2026) */

/** How a login ended, it decides whether to try again. */
typedef enum {
  LOGIN_OK,          /**< the server accepted the account */
  LOGIN_REJECTED,    /**< HTTP 401 or Success:false, no new try until the next start */
  LOGIN_RETRY        /**< anything else, tried again after a pause */
} login_t;

/** How submitting an unlock ended. */
typedef enum {
  SUBMIT_DONE,       /**< the server has it, also as "User already has" */
  SUBMIT_PARK,       /**< refused for good, it goes to the parked file */
  SUBMIT_REQUEUE,    /**< refused, but it may pass later, it goes behind the others */
  SUBMIT_RETRY       /**< no answer from the server, tried again after a pause */
} submit_t;

static TaskHandle_t      task;             // NULL until ra_task_start()
static StackType_t       task_stack[RA_TASK_STACK];   // outside the FreeRTOS heap, see ra_task_start()
static StaticTask_t      task_tcb;
static QueueSetHandle_t  events;           // everything the task waits for
static SemaphoreHandle_t clock_sem;        // given when NTP has set the clock
static QueueHandle_t     unlocks;          // com_task -> RA task, see ra_queue.c
static char              reply_buf[2048];  // replies to the small requests, the longest is a list of unlocked ids
static const char       *user, *token;     // [RA] in config.ini, the token is never logged
static bool              account;          // both are set, only then unlocks are kept
static volatile ra_task_state_t state = RA_TASK_STARTING;   // written here, read by com_task

ra_task_state_t ra_task_state(void) { return state; }

void ra_task_clock_set(void) {
  if(clock_sem) xSemaphoreGive(clock_sem);  // the task checks time() itself when it wakes
}

/* Handles one event of the queue set: an unlock goes to the card, the clock only
   wakes the task. Also called by ra_net_get() while a request runs. */
static void on_event(QueueSetMemberHandle_t m) {
  if(m == unlocks)        ra_queue_take(account);
  else if(m == clock_sem) xSemaphoreTake(clock_sem, 0);
}

/* Waits up to ticks for the next event and handles it. false when the time ran out. */
static bool wait_event(TickType_t ticks) {
  QueueSetMemberHandle_t m = xQueueSelectFromSet(events, ticks);
  if(!m) return false;
  on_event(m);
  return true;
}

/* A pause that still handles every event. */
static void pause_ms(uint32_t ms) {
  TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
  for(;;) {
    TickType_t left = end - xTaskGetTickCount();
    if((int32_t)left <= 0) return;
    wait_event(left);
  }
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
  params.game_hash = ra_game_hash();
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
    debugf("RA: server reached, hash %s is game %u", ra_game_hash(), (unsigned)response.game_id);
    ok = true;
  }
  rc_api_destroy_resolve_hash_response(&response);
  return ok;
}

/* Checks the account with r=ping. Only HTTP 401, or Success:false in a 200
   reply, count as rejected. Everything else, from DNS to a 5xx, is tried again. */
static login_t login(void) {
  rc_api_ping_request_t params;
  rc_api_request_t request;
  char path[128];
  ra_reply_t reply;
  login_t result = LOGIN_RETRY;

  // rcheevos builds the query (r=ping&u=<user>&t=<token>&g=<id>), it goes out as GET
  memset(&params, 0, sizeof(params));
  params.username  = user;
  params.api_token = token;
  params.game_id   = ra_game_id();
  if(rc_api_init_ping_request(&request, &params) != RC_OK) return LOGIN_RETRY;
  snprintf(path, sizeof(path), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);

  if(ra_net_get(path, reply_buf, sizeof(reply_buf), &reply) != 0 || reply.result != 0)
    return LOGIN_RETRY;
  if(reply.status != 200 && reply.status != 401 && reply.status != 403) return LOGIN_RETRY;

  // rcheevos reads the reply, only its error text goes to the log
  rc_api_ping_response_t response;
  rc_api_server_response_t server;
  memset(&server, 0, sizeof(server));
  server.body = reply_buf;
  server.body_length = reply.len;
  server.http_status_code = (int)reply.status;
  int rv = rc_api_process_ping_server_response(&response, &server);
  const char *why = response.response.error_message ? response.response.error_message : "no reason given";
  if(reply.status == 200 && rv == RC_OK && response.response.succeeded)
    result = LOGIN_OK;
  else if(reply.status == 403)
    // from RA itself (JSON, e.g. a blocked client) or from Cloudflare (a page,
    // e.g. for the User-Agent). Tried again either way, the log tells which.
    debugf("RA: refused by the server (HTTP 403, %s)%s", why, rv == RC_OK ? "" : ", check the User-Agent");
  else if(reply.status == 401 || rv == RC_OK) {
    debugf("RA: login rejected (%s), achievements stay local, check [RA] in config.ini", why);
    result = LOGIN_REJECTED;
  }
  rc_api_destroy_ping_response(&response);
  return result;
}

/* Sends one unlock with r=awardachievement. It is parked only when the server
   refuses it in a normal reply and the id is no longer in the active set, so a
   bad token or a server error never costs a real unlock. Any other refusal moves
   it behind the others, so it cannot hold them up. */
static submit_t submit(const ra_unlock_t *u) {
  rc_api_award_achievement_request_t params;
  rc_api_request_t request;
  char path[320];
  ra_reply_t reply;
  submit_t result = SUBMIT_RETRY;
  unsigned long now = (unsigned long)time(NULL);

  // rcheevos builds the query with its signature. Hardcore: the machine has no
  // save states or rewind, and while game20k is not a known client the server
  // counts it as softcore anyway. Without a time the server takes its own.
  memset(&params, 0, sizeof(params));
  params.username       = user;
  params.api_token      = token;
  params.achievement_id = u->id;
  params.hardcore       = 1;
  params.game_hash      = ra_game_hash();
  if(u->when && now >= u->when) params.seconds_since_unlock = now - u->when;
  if(rc_api_init_award_achievement_request(&request, &params) != RC_OK) return SUBMIT_RETRY;
  int n = snprintf(path, sizeof(path), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);
  if(n < 0 || n >= (int)sizeof(path)) {
    debugf("RA: request for unlock %u too long", u->id);
    return SUBMIT_RETRY;
  }

  if(ra_net_get(path, reply_buf, sizeof(reply_buf), &reply) != 0 ||
     reply.result != 0 || reply.status != 200)
    return SUBMIT_RETRY;

  // rcheevos reads the reply and turns "User already has" into success
  rc_api_award_achievement_response_t response;
  rc_api_server_response_t server;
  memset(&server, 0, sizeof(server));
  server.body = reply_buf;
  server.body_length = reply.len;
  server.http_status_code = (int)reply.status;
  int rv = rc_api_process_award_achievement_server_response(&response, &server);
  const char *why = response.response.error_message ? response.response.error_message : "no reason given";
  if(rv == RC_OK && response.response.succeeded) {
    if(response.response.error_message)
      debugf("RA: unlock %u already on the account", u->id);
    else
      debugf("RA: unlock %u confirmed, score %u, softcore %u, %u left", u->id,
             (unsigned)response.new_player_score, (unsigned)response.new_player_score_softcore,
             (unsigned)response.achievements_remaining);
    result = SUBMIT_DONE;
  } else if(rv == RC_OK) {
    // the set belongs to com_task, it is only read here
    if(ra_patch_count() && !ra_patch_index(u->id)) {
      debugf("RA: unlock %u refused (%s), not in the set any more, parked", u->id, why);
      result = SUBMIT_PARK;
    } else {
      debugf("RA: unlock %u refused (%s), moved behind the others", u->id, why);
      result = SUBMIT_REQUEUE;
    }
  }
  rc_api_destroy_award_achievement_response(&response);
  return result;
}

/* Asks the server for one of the account's lists with r=unlocks, h=1 the
   hardcore one and h=0 the softcore one. A reply that did not fit into the
   buffer is not used, its list would be incomplete. The caller destroys
   response in every case. */
static bool fetch_list(bool hardcore, rc_api_fetch_user_unlocks_response_t *response) {
  rc_api_fetch_user_unlocks_request_t params;
  rc_api_request_t request;
  char path[128];
  ra_reply_t reply;

  memset(&params, 0, sizeof(params));
  params.username  = user;
  params.api_token = token;
  params.game_id   = ra_game_id();
  params.hardcore  = hardcore;
  if(rc_api_init_fetch_user_unlocks_request(&request, &params) != RC_OK) return false;
  snprintf(path, sizeof(path), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);

  if(ra_net_get(path, reply_buf, sizeof(reply_buf), &reply) != 0 ||
     reply.result != 0 || reply.status != 200 || reply.truncated)
    return false;

  // rcheevos reads the list, only its error text goes to the log
  rc_api_server_response_t server;
  memset(&server, 0, sizeof(server));
  server.body = reply_buf;
  server.body_length = reply.len;
  server.http_status_code = (int)reply.status;
  int rv = rc_api_process_fetch_user_unlocks_server_response(response, &server);
  if(rv == RC_OK && response->response.succeeded) return true;
  debugf("RA: %s list unusable (%s)", hardcore ? "hardcore" : "softcore",
         response->response.error_message ? response->response.error_message :
         rv != RC_OK ? rc_error_str(rv) : "marked as failed");
  return false;
}

/* Both lists from the server, or none: the state never mixes a new list with
   an old one. On success both are kept on the card. */
static bool fetch_state(void) {
  rc_api_fetch_user_unlocks_response_t hard, soft;
  memset(&hard, 0, sizeof(hard));     // destroy is safe on a zeroed response
  memset(&soft, 0, sizeof(soft));
  bool ok = fetch_list(true, &hard) && fetch_list(false, &soft);
  if(ok) {
    ra_state_replace(true,  hard.achievement_ids, hard.num_achievement_ids);
    ra_state_replace(false, soft.achievement_ids, soft.num_achievement_ids);
    ra_state_save();
  }
  rc_api_destroy_fetch_user_unlocks_response(&hard);
  rc_api_destroy_fetch_user_unlocks_response(&soft);
  return ok;
}

/* Pause before the next try. It doubles each time, up to RA_BACKOFF_MAX. */
static void wait_to_retry(uint32_t *backoff, const char *what) {
  debugf("RA: %s, next try in %lu s", what, (unsigned long)(*backoff / 1000));
  pause_ms(*backoff);
  *backoff = *backoff < RA_BACKOFF_MAX / 2 ? *backoff * 2 : RA_BACKOFF_MAX;
}

/* Nothing more to do on the server. Unlocks are still kept. */
static void sleep_forever(void) {
  for(;;) wait_event(portMAX_DELAY);
}

static void ra_task_main(__attribute__((unused)) void *p) {
  uint32_t backoff = RA_BACKOFF_MIN;
  char what[40];

  debugf("RA: client %s", ra_user_agent());

  // 1. without an account there is nothing to do on the server, and unlocks are
  //    not kept: a guest's would later count for the owner
  user    = inifile_config_get_str("ra", "user");
  token   = inifile_config_get_str("ra", "token");
  account = user && *user && token && *token;
  if(!account) {
    debugf("RA: no account in config.ini, achievements stay local");
    state = RA_TASK_NO_ACCOUNT;
    sleep_forever();
  }
  ra_state_load(user);      // before the queue, its lines count as unlocked too
  ra_queue_open(user);

  // 2. certificates are checked against the clock, so wait until NTP has set it.
  //    Without a time server this can take forever, so say it once in the log.
  TickType_t tell = xTaskGetTickCount() + pdMS_TO_TICKS(RA_CLOCK_WAIT);
  bool told = false;
  state = RA_TASK_CONNECTING;
  while((unsigned long)time(NULL) < RA_CLOCK_VALID) {
    TickType_t left = tell - xTaskGetTickCount();
    if(!told && (int32_t)left <= 0) {
      debugf("RA: no time from NTP yet, achievements wait for it (see [NTP] IP in config.ini)");
      told = true;            // said once, now sleep until the clock is set
      state = RA_TASK_NO_TIME;
    }
    wait_event(told ? portMAX_DELAY : left);
  }
  state = RA_TASK_CONNECTING;

  // 3. reach the server, with growing pauses while it or the network is down
  while(!check_server()) {
    state = RA_TASK_RETRYING;
    wait_to_retry(&backoff, "server not reached");
    state = RA_TASK_CONNECTING;
  }

  // 4. log in. A rejected account stays rejected until the next start.
  debugf("RA: logging in as '%s' (token %u characters, not shown)", user, (unsigned)strlen(token));
  backoff = RA_BACKOFF_MIN;
  for(;;) {
    login_t r = login();
    if(r == LOGIN_OK) break;
    if(r == LOGIN_REJECTED) { state = RA_TASK_REJECTED; sleep_forever(); }
    state = RA_TASK_RETRYING;
    wait_to_retry(&backoff, "login failed");
    state = RA_TASK_CONNECTING;
  }
  debugf("RA: logged in");
  state = RA_TASK_LOGGED_IN;

  // 5. the account's lists from the server, then the waiting unlocks one by
  //    one, then wait for the next. Once the queue is empty after a submission
  //    changed the lists, they are asked again. When they cannot be fetched,
  //    the state from the card stays, the unlocks still go out, and the lists
  //    are asked again after a pause.
  bool state_due = true, state_failed = false, state_stale = false;
  backoff = RA_BACKOFF_MIN;
  for(;;) {
    ra_unlock_t u;
    if(state_due) {
      state_due = false;
      state_failed = !fetch_state();
      if(!state_failed) backoff = RA_BACKOFF_MIN;
    }
    int h = ra_queue_head(&u);
    if(h == 0) {
      if(state_stale) {
        state_stale = false;          // the lists changed, ask once more
        state_due = true;
      } else if(state_failed) {
        // ask again after a pause. A new unlock ends the pause, it does not wait
        debugf("RA: unlocked state not available, the card state stays, next try in %lu s",
               (unsigned long)(backoff / 1000));
        state_failed = false;
        state_due = true;
        wait_event(pdMS_TO_TICKS(backoff));
        backoff = backoff < RA_BACKOFF_MAX / 2 ? backoff * 2 : RA_BACKOFF_MAX;
      } else
        wait_event(portMAX_DELAY);
      continue;
    }
    if(h < 0) {
      wait_to_retry(&backoff, "queue on the card not readable");
      continue;
    }
    submit_t r = submit(&u);
    bool kept = r == SUBMIT_DONE    ? ra_queue_pop()     :
                r == SUBMIT_PARK    ? ra_queue_park()    :
                r == SUBMIT_REQUEUE ? ra_queue_requeue() : false;
    if(r == SUBMIT_RETRY)
      snprintf(what, sizeof(what), "unlock %u not submitted", u.id);
    else if(!kept)
      snprintf(what, sizeof(what), "unlock %u: card not updated", u.id);
    else if(r == SUBMIT_REQUEUE)
      snprintf(what, sizeof(what), "unlock %u refused", u.id);
    else {
      backoff = RA_BACKOFF_MIN;
      state_stale = true;             // DONE or PARK: the server's lists may differ now
      continue;
    }
    // a pause also after a refusal, so a queue of refused unlocks cannot hammer the server
    wait_to_retry(&backoff, what);
  }
}

void ra_task_start(void) {
  if(task) return;
  // one queue set for everything the task waits for, created before com_task
  // can report an unlock
  events    = xQueueCreateSet(RA_EVENTS);
  clock_sem = xSemaphoreCreateBinary();
  unlocks   = ra_queue_init();
  if(!events || !clock_sem || !unlocks ||
     xQueueAddToSet(unlocks, events) != pdPASS || xQueueAddToSet(clock_sem, events) != pdPASS ||
     !ra_net_init(events, on_event)) {
    debugf("RA: events could not be set up");
    return;
  }
  // below com_task, like wifi_task. The stack is a static array and not taken from
  // the FreeRTOS heap (112 KB): that is nearly full once an FTP session runs
  // (12 KB), and 8 KB more of it left FTP no room to open a data connection
  // (measured 26.09.2026)
  task = xTaskCreateStatic(ra_task_main, "RA", RA_TASK_STACK, NULL, configMAX_PRIORITIES - 10,
                           task_stack, &task_tcb);
  if(!task) debugf("RA: task could not be created");
}
