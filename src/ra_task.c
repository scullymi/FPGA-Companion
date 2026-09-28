/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_task.c
 *  @brief The RetroAchievements task: all network work for the achievements.
 *
 *  It runs apart from com_task, so the game loop never waits for the server. It
 *  waits on one queue set for everything that can wake it: unlocks from com_task,
 *  the NTP clock and, inside ra_net_get(), the end of a request. So an unlock is
 *  written to the card at once, even while a request is running.
 *
 *  After the login it starts a session and then pings every two minutes with
 *  the rich presence text, as rc_client does, so the server shows what is being
 *  played and keeps the session. */
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
#include "sysctrl.h"
#include "ra_net.h"
#include "ra_patch.h"
#include "ra_queue.h"
#include "ra_state.h"
#include "ra_task.h"

#define RA_CLOCK_WAIT   60000u        /**< ms without time from NTP before the log says so */
#define RA_BACKOFF_MIN  10000u        /**< ms, first pause after a failed request */
#define RA_BACKOFF_MAX  600000u       /**< ms, the pause doubles up to this */
#define RA_EVENTS       (RA_QUEUE_HANDOVER + 2)   /**< queue set: the unlocks, the clock, the end of a request */
#define RA_TASK_STACK   2048          /**< words, a TLS handshake leaves about 5 KB of it free */
#define RA_PING_FIRST   30000u        /**< ms from the session start to the first ping, as rc_client */
#define RA_PING_EVERY   120000u       /**< ms between pings, the server keeps the session meanwhile */
#define RA_PATH_MAX     1024          /**< a ping with the rich presence text, url-encoded at three characters per byte */

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
static volatile bool     hardcore;         // the mode of session, pings and unlocks, see ra_task_hardcore()
static volatile bool     hardcore_due;     // hardcore asked for, it applies when the reset for it ends
static volatile bool     core_running;     // the core is out of reset, a game may be running
static volatile bool     hc_wanted;        // the menu's 'H'
static volatile unsigned hc_block;         // RA_HC_BLOCK_* reasons that keep hardcore off
static volatile unsigned char core_flags;  // header byte 9 of the RAM mirror, 0 = release build
static char              rp_text[RA_RP_MAX];   // rich presence from com_task, under a critical section
static volatile unsigned frames;           // frames rcheevos evaluated, counted by com_task
static char              path_buf[RA_PATH_MAX];  // path of session and ping requests, too large for the stack
static bool              live;             // logged in: session and pings are due
static TickType_t        next_ping;        // when the next ping, or the next try of the session, is due

ra_task_state_t ra_task_state(void) { return state; }
bool ra_task_hardcore(void) { return hardcore; }

unsigned ra_task_hardcore_blocked(void) { return hc_block; }
bool ra_task_hardcore_wanted(void) { return hc_wanted; }

/* Brings the mode in line with what the menu wants and what blocks it. Softcore
   applies at once. Hardcore applies at once while the core is in reset, e.g. at
   the start, otherwise the running game is reset first and the mode applies when
   the reset ends: a game that started in softcore never continues in hardcore. */
static void hc_update(void) {
  if(!hc_wanted || hc_block) {
    if(hardcore || hardcore_due) debugf("RA: softcore from now on");
    hardcore = hardcore_due = false;
    return;
  }
  if(hardcore || hardcore_due) return;
  if(!core_running) {
    hardcore = true;          // no game runs, the next one starts in hardcore
    return;
  }
  debugf("RA: hardcore, the game is reset");
  hardcore_due = true;
  sys_set_val('R', 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  sys_set_val('R', 0);
}

void ra_task_hardcore_block(unsigned reason, bool on) {
  unsigned before = hc_block;
  hc_block = on ? (before | reason) : (before & ~reason);
  if(hc_block != before) hc_update();
}

void ra_task_core_flags(unsigned char flags) {
  if(flags == core_flags) return;
  core_flags = flags;
  if(flags) debugf("RA: the core was built for diagnostics (flags 0x%02x), no hardcore", flags);
  ra_task_hardcore_block(RA_HC_BLOCK_CORE, flags != 0);
}

void ra_task_core_value(char id, int value) {
  if(id == 'R') {
    core_running = !(value & 1);
    if(!core_running) return;
    // a reset ends: rcheevos starts over before the next frame, and the game
    // starts anew, in hardcore if that was asked for. The reset is flagged first:
    // com_task runs above this task and must not evaluate a frame in hardcore
    // with the hit counts of the game before.
    ra_patch_core_reset();
    if(hardcore_due) {
      hardcore_due = false;
      hardcore = true;
      debugf("RA: hardcore from this game on");
    }
  } else if(id == 'H') {
    hc_wanted = value != 0;
    hc_update();
  }
}
void ra_task_frame(void) { frames++; }

void ra_task_set_richpresence(const char *text) {
  size_t n = strnlen(text, RA_RP_MAX - 1);
  // a short copy with interrupts off, the RA task copies it the same way
  taskENTER_CRITICAL();
  memcpy(rp_text, text, n);
  rp_text[n] = 0;
  taskEXIT_CRITICAL();
}

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

static void keep_alive(void);

/* A pause that still handles every event, and keeps the session alive once logged in. */
static void pause_ms(uint32_t ms) {
  TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
  for(;;) {
    keep_alive();
    TickType_t now = xTaskGetTickCount();
    TickType_t left = end - now;
    if((int32_t)left <= 0) return;
    // wake for the next ping too, when it comes first. One already due wakes at
    // once, keep_alive() sends it at the top of the loop.
    int32_t to_ping = (int32_t)(next_ping - now);
    if(live && to_ping < (int32_t)left) left = to_ping > 0 ? (TickType_t)to_ping : 0;
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

/* Reads a reply into rcheevos' server response. */
static void server_reply(rc_api_server_response_t *server, const ra_reply_t *reply) {
  memset(server, 0, sizeof(*server));
  server->body = reply_buf;
  server->body_length = reply->len;
  server->http_status_code = (int)reply->status;
}

/* Starts the session with r=startsession: the server notes that the game is
   played, with which hash and in which mode. false when it did not take it, the
   caller tries again at the next ping time. */
static bool start_session(void) {
  rc_api_start_session_request_t params;
  rc_api_request_t request;
  rc_api_server_response_t server;
  ra_reply_t reply;

  memset(&params, 0, sizeof(params));
  params.username  = user;
  params.api_token = token;
  params.game_id   = ra_game_id();
  params.game_hash = ra_game_hash();
  params.hardcore  = hardcore;
  if(rc_api_init_start_session_request(&request, &params) != RC_OK) return false;
  int n = snprintf(path_buf, sizeof(path_buf), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);
  if(n < 0 || n >= (int)sizeof(path_buf)) return false;

  if(ra_net_get(path_buf, reply_buf, sizeof(reply_buf), &reply) != 0 ||
     reply.result != 0 || reply.status != 200)
    return false;
  // the reply also lists the account's unlocks, which r=unlocks fetches as well.
  // A list too long for the buffer only cuts the reply, the session stands.
  if(reply.truncated) {
    debugf("RA: session started (%s), reply cut short", hardcore ? "hardcore" : "softcore");
    return true;
  }
  rc_api_start_session_response_t response;
  server_reply(&server, &reply);
  int rv = rc_api_process_start_session_server_response(&response, &server);
  bool ok = rv == RC_OK && response.response.succeeded;
  if(ok)
    debugf("RA: session started (%s)", hardcore ? "hardcore" : "softcore");
  else
    debugf("RA: session not started (%s)", response.response.error_message ?
           response.response.error_message : rv != RC_OK ? rc_error_str(rv) : "marked as failed");
  rc_api_destroy_start_session_response(&response);
  return ok;
}

/* Pings with r=ping: keeps the session and sends the rich presence text, the
   game hash and the mode. The log shows the text only when it changed. */
static void ping(void) {
  static char rp[RA_RP_MAX];          // the text this ping sends
  static char rp_logged[RA_RP_MAX];   // the text the log showed last
  rc_api_ping_request_t params;
  rc_api_request_t request;
  rc_api_server_response_t server;
  ra_reply_t reply;

  taskENTER_CRITICAL();
  memcpy(rp, rp_text, sizeof(rp));
  taskEXIT_CRITICAL();

  memset(&params, 0, sizeof(params));
  params.username      = user;
  params.api_token     = token;
  params.game_id       = ra_game_id();
  params.rich_presence = rp[0] ? rp : NULL;
  params.game_hash     = ra_game_hash();
  params.hardcore      = hardcore;
  if(rc_api_init_ping_request(&request, &params) != RC_OK) return;
  int n = snprintf(path_buf, sizeof(path_buf), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);
  if(n < 0 || n >= (int)sizeof(path_buf)) {
    debugf("RA: ping too long, not sent");
    return;
  }

  // ra_net_get() logs the request, only a refusal needs a word more
  if(ra_net_get(path_buf, reply_buf, sizeof(reply_buf), &reply) != 0 ||
     reply.result != 0 || reply.status != 200)
    return;
  rc_api_ping_response_t response;
  server_reply(&server, &reply);
  int rv = rc_api_process_ping_server_response(&response, &server);
  if(rv != RC_OK || !response.response.succeeded)
    debugf("RA: ping refused (%s)", response.response.error_message ?
           response.response.error_message : rv != RC_OK ? rc_error_str(rv) : "marked as failed");
  else if(strcmp(rp, rp_logged)) {
    debugf("RA: rich presence '%s'", rp);
    memcpy(rp_logged, rp, sizeof(rp_logged));
  }
  rc_api_destroy_ping_response(&response);
}

/* Once logged in: at each ping time the session, while the server has not taken
   it, else a ping, but only when frames arrived since the last one. Without
   frames the server lets the session end, as it does with rc_client. */
static void keep_alive(void) {
  static bool     session;       // the server took the session
  static unsigned frames_seen;   // frames at the last ping
  if(!live) return;
  TickType_t now = xTaskGetTickCount();
  if((int32_t)(now - next_ping) < 0) return;
  next_ping = now + pdMS_TO_TICKS(RA_PING_EVERY);
  if(!session) {
    session = start_session();
    if(session) next_ping = xTaskGetTickCount() + pdMS_TO_TICKS(RA_PING_FIRST);
  } else if(frames != frames_seen) {
    frames_seen = frames;
    ping();
  }
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

  // rcheevos builds the query with its signature. The mode is the one the unlock
  // was earned in. Without a time the server takes its own.
  memset(&params, 0, sizeof(params));
  params.username       = user;
  params.api_token      = token;
  params.achievement_id = u->id;
  params.hardcore       = u->hardcore;
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

/* Asks the server for the game's set with r=patch, straight into ra_patch's
   buffer, and lets ra_patch keep and apply it. false when no usable set came
   back, for whatever reason: the caller asks again after a pause. A reply too
   large for the buffer is the one thing that would not change, it is left. */
static bool fetch_set(void) {
  rc_api_fetch_game_data_request_t params;
  rc_api_request_t request;
  char path[128];
  ra_reply_t reply;
  unsigned cap;
  char *buf = ra_patch_body(&cap);

  memset(&params, 0, sizeof(params));
  params.username  = user;
  params.api_token = token;
  params.game_id   = ra_game_id();
  if(rc_api_init_fetch_game_data_request(&request, &params) != RC_OK) return false;
  snprintf(path, sizeof(path), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);

  if(ra_net_get(path, buf, cap, &reply) != 0 || reply.result != 0 || reply.status != 200)
    return false;
  if(reply.truncated) {
    debugf("RA: set from the server is larger than %u bytes, not used", cap - 1);
    return true;
  }
  return ra_patch_from_server(reply.len) >= 0;
}

/* Both lists from the server, or none: the state never mixes a new list with
   an old one. On success both are kept on the card. */
static bool fetch_state(void) {
  rc_api_fetch_user_unlocks_response_t hard, soft;
  memset(&hard, 0, sizeof(hard));     // destroy is safe on a zeroed response
  memset(&soft, 0, sizeof(soft));
  bool ok = fetch_list(true, &hard) && fetch_list(false, &soft);
  if(ok) {
    // the card only when something changed, a write holds the SPI bus for the mirror
    bool changed = ra_state_replace(true, hard.achievement_ids, hard.num_achievement_ids);
    changed |= ra_state_replace(false, soft.achievement_ids, soft.num_achievement_ids);
    if(changed) ra_state_save();
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

  // 1. the set from the card, for com_task. The achievements run without an
  //    account too, only nothing goes to the server then.
  ra_patch_read_card();

  // 2. without an account there is nothing to do on the server, and unlocks are
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

  // 3. certificates are checked against the clock, so wait until NTP has set it.
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

  // 4. reach the server, with growing pauses while it or the network is down
  while(!check_server()) {
    state = RA_TASK_RETRYING;
    wait_to_retry(&backoff, "server not reached");
    state = RA_TASK_CONNECTING;
  }

  // 5. log in. A rejected account stays rejected until the next start.
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
  live = true;
  next_ping = xTaskGetTickCount();   // the session right away

  // 6. the account's lists and the game's set from the server, then the
  //    waiting unlocks one by one, then wait for the next. Once the queue is
  //    empty after a submission changed the lists, they are asked again. When
  //    the server does not answer, what the card has stays, the unlocks still
  //    go out, and the question is asked again after a pause.
  bool state_due = true, state_failed = false, state_stale = false;
  bool set_due = true, set_failed = false;
  TickType_t retry_at = 0;            // when failed fetches are asked again
  bool       retry_set = false;       // ... and that time is set
  backoff = RA_BACKOFF_MIN;
  for(;;) {
    ra_unlock_t u;
    keep_alive();
    if(state_due) {
      state_due = false;
      state_failed = !fetch_state();
      if(!state_failed) backoff = RA_BACKOFF_MIN;
    }
    if(set_due) {
      set_due = false;
      set_failed = !fetch_set();
      if(!set_failed) backoff = RA_BACKOFF_MIN;
    }
    // both fetched, by whatever path: a retry time from an earlier failure is void
    if(!state_failed && !set_failed) retry_set = false;
    int h = ra_queue_head(&u);
    if(h == 0) {
      TickType_t now = xTaskGetTickCount();
      TickType_t wake = next_ping;    // nothing to send: sleep until the next ping at most
      if(state_stale) {
        state_stale = false;          // the lists changed, ask once more
        state_due = true;
        continue;
      }
      if(state_failed || set_failed) {
        // ask again after a pause. A new unlock ends the wait, it goes out first
        if(!retry_set) {
          debugf("RA: %s not fetched, what the card has stays, next try in %lu s",
                 state_failed && set_failed ? "unlocked state and set" : state_failed ? "unlocked state" : "set",
                 (unsigned long)(backoff / 1000));
          retry_at  = now + pdMS_TO_TICKS(backoff);
          retry_set = true;
          backoff = backoff < RA_BACKOFF_MAX / 2 ? backoff * 2 : RA_BACKOFF_MAX;
        }
        if((int32_t)(now - retry_at) >= 0) {
          state_due = state_failed;
          set_due   = set_failed;
          state_failed = set_failed = false;
          retry_set = false;
          continue;
        }
        if((int32_t)(retry_at - wake) < 0) wake = retry_at;
      }
      if((int32_t)(wake - now) > 0) wait_event(wake - now);
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
    // a fetch that failed is due again after that pause too, so a refused unlock
    // cannot keep the lists or the set from being asked for
    state_due = state_due || state_failed;
    set_due   = set_due   || set_failed;
    state_failed = set_failed = false;
    retry_set = false;
  }
}

void ra_task_start(void) {
  if(task) return;
  // one queue set for everything the task waits for, created before com_task
  // can report an unlock
  events    = xQueueCreateSet(RA_EVENTS);
  clock_sem = xSemaphoreCreateBinary();
  unlocks   = ra_queue_init();
  if(!events || !clock_sem || !unlocks || !ra_patch_init() ||
     xQueueAddToSet(unlocks, events) != pdPASS || xQueueAddToSet(clock_sem, events) != pdPASS ||
     !ra_net_init(events, on_event)) {
    debugf("RA: events could not be set up");
    return;
  }
  // below com_task, like wifi_task. The stack is a static array and not taken from
  // the FreeRTOS heap: that heap (112 KB) is nearly full once an FTP session runs
  // (12 KB), and 8 KB more would leave FTP no room for a data connection
  task = xTaskCreateStatic(ra_task_main, "RA", RA_TASK_STACK, NULL, configMAX_PRIORITIES - 10,
                           task_stack, &task_tcb);
  if(!task) debugf("RA: task could not be created");
}
