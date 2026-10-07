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
#include "ra_mac.h"

#define RA_CLOCK_WAIT   60000u        /**< ms without time from NTP before the log says so */
#define RA_BACKOFF_MIN  10000u        /**< ms, first pause after a failed request */
#define RA_BACKOFF_MAX  600000u       /**< ms, the pause doubles up to this */
#define RA_LB_QUEUE     4             /**< leaderboard results com_task can hand over before the RA task takes them */
#define RA_LB_RAM       4             /**< leaderboard results kept until the server has them */
#define RA_LB_MAX_AGE   (14u * 24u * 3600u)   /**< s, older results the server would not take with their time */
#define RA_EVENTS       (RA_QUEUE_HANDOVER + 2 + RA_LB_QUEUE)   /**< queue set: unlocks, the clock, the end of a request, leaderboards */
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
// RA_HC_BLOCK_* reasons that keep hardcore off. ROM, SET and GAME hold from the
// start until ra_patch_settle() has checked ROM image and board and the
// achievement set has been checked: nothing is granted before the game is known.
static volatile unsigned hc_block = RA_HC_BLOCK_ROM | RA_HC_BLOCK_SET | RA_HC_BLOCK_GAME;
static volatile unsigned char core_flags;  // header byte 9 of the RAM mirror, 0 = release build
static char              rp_text[RA_RP_MAX];   // rich presence from com_task, under a critical section
static volatile unsigned frames;           // frames rcheevos evaluated, counted by com_task
static char              path_buf[RA_PATH_MAX];  // path of session and ping requests, too large for the stack
static bool              live;             // logged in: session and pings are due
static TickType_t        next_ping;        // when the next ping, or the next try of the session, is due

/** A leaderboard result on its way to the server. It lives in RAM for this boot
    only, and the game's identity cannot change within a boot, so the running
    game's hash is the hash it was earned under. */
typedef struct {
  unsigned   id;       /**< leaderboard id */
  int32_t    score;    /**< the value */
  TickType_t tick;     /**< when it was reached, for seconds_since_completion */
} lb_entry_t;
static QueueHandle_t      lb_queue;                          // com_task -> RA task
static StaticQueue_t      lb_queue_ctl;
static uint8_t            lb_queue_mem[RA_LB_QUEUE * sizeof(lb_entry_t)];
static lb_entry_t         lb_ram[RA_LB_RAM];                 // waiting for the server, oldest first
static volatile unsigned  lb_n;                              // volatile: ra_task_lboard_pending() reads it from com_task
static volatile bool      lb_taking;                         // lb_take() holds a result between the queue and lb_ram[]
static ra_lboard_result_t lb_result;                         // the server's latest answer
static volatile unsigned  lb_seq;                            // counts answers, see ra_task_lboard_result()

#define RA_REFUSE_MAX 3   /**< refusals of a queued unlock of another game before it is parked, see refusals() */
#define RA_REFUSE_MEM 8   /**< such lines counted at once, beyond that a line is parked at its first refusal */
/** A queued unlock of another game the server refused, counted per boot. */
typedef struct {
  unsigned id;                          /**< achievement id */
  char     hash[RA_GAMES_HASH_LEN + 1]; /**< the game it was earned under */
  unsigned n;                           /**< refusals so far, 0 for a free slot */
} refusal_t;
static refusal_t refused[RA_REFUSE_MEM];
static bool      set_in_flight;         // fetch_set() has the server's reply on its way into ra_patch's body[]
static volatile bool set_reread_failed; // a card set asked for again was not there: the server is asked once more

ra_task_state_t ra_task_state(void) { return state; }
bool ra_task_hardcore(void) { return hardcore && hc_wanted && !hc_block; }

unsigned ra_task_hardcore_blocked(void) { return hc_block; }
bool ra_task_hardcore_wanted(void) { return hc_wanted; }

/* Brings the mode in line with what the menu wants and what blocks it. Softcore
   applies at once. Hardcore applies at once while the core is in reset, e.g. at
   the start, otherwise the running game is reset first and the mode applies when
   the reset ends: a game that started in softcore never continues in hardcore. */
static void hc_update(void) {
  bool to_soft = false, reset = false;
  // menu_task, com_task and the RA task all get here: the decision is made with
  // interrupts off, so no two of them can each take half of it. Log and reset
  // come after, outside.
  taskENTER_CRITICAL();
  if(!hc_wanted || hc_block) {
    to_soft = hardcore || hardcore_due;
    hardcore = hardcore_due = false;
  } else if(!hardcore && !hardcore_due) {
    if(!core_running)
      hardcore = true;          // no game runs, the next one starts in hardcore
    else {
      hardcore_due = true;      // a running game is reset first, see the R=0 path
      reset = true;
    }
  }
  taskEXIT_CRITICAL();
  if(to_soft) debugf("RA: softcore from now on");
  if(reset) {
    debugf("RA: hardcore, the game is reset");
    sys_set_val('R', 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    sys_set_val('R', 0);
  }
}

void ra_task_hardcore_block(unsigned reason, bool on) {
  unsigned before, after;
  taskENTER_CRITICAL();
  before = hc_block;
  after = hc_block = on ? (before | reason) : (before & ~reason);
  taskEXIT_CRITICAL();
  if(after != before) hc_update();
}

void ra_task_core_flags(unsigned char flags) {
  // set on every snapshot, not only on a change: a bit lost to another task's
  // update would otherwise never come back
  if(flags != core_flags && flags)
    debugf("RA: the core was built for diagnostics (flags 0x%02x), no hardcore", flags);
  core_flags = flags;
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
    bool on;
    taskENTER_CRITICAL();
    on = hardcore_due && hc_wanted && !hc_block;   // a reason may have come up meanwhile
    hardcore_due = false;
    if(on) hardcore = true;
    taskEXIT_CRITICAL();
    if(on) debugf("RA: hardcore from this game on");
  } else if(id == 'H') {
    hc_wanted = value != 0;
    hc_update();
  }
}
unsigned ra_task_lboard_pending(void) {
  // in this order: a result leaves the queue only after lb_taking is set, and
  // lb_taking is cleared only once the result is in lb_ram[]
  unsigned n = lb_queue ? (unsigned)uxQueueMessagesWaiting(lb_queue) : 0;
  if(lb_taking) n++;
  return n + lb_n;
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

void ra_task_wake(void) {
  if(clock_sem) xSemaphoreGive(clock_sem);  // the task looks around itself when it wakes
}

void ra_task_clock_set(void) {
  ra_task_wake();                           // the task checks time() itself
}

void ra_task_lboard(unsigned id, int32_t score) {
  lb_entry_t e = { id, score, xTaskGetTickCount() };
  if(!lb_queue || xQueueSend(lb_queue, &e, 0) != pdTRUE)
    debugf("RA: leaderboard %u result lost, the RA task does not take it", id);
}

bool ra_task_lboard_result(unsigned *seen, ra_lboard_result_t *out) {
  bool newer;
  taskENTER_CRITICAL();
  newer = lb_seq != *seen;
  if(newer) { *out = lb_result; *seen = lb_seq; }
  taskEXIT_CRITICAL();
  return newer;
}

/* Takes a leaderboard result from com_task into the RAM list. Without an account
   it is dropped, as unlocks are. When the list is full the oldest goes. */
static void lb_take_one(void) {
  lb_entry_t e;
  if(!lb_queue || xQueueReceive(lb_queue, &e, 0) != pdTRUE) return;
  if(!account) {
    debugf("RA: leaderboard %u result not kept, no account", e.id);
    return;
  }
  if(lb_n == RA_LB_RAM) {
    debugf("RA: leaderboard %u result dropped, %u newer ones wait", lb_ram[0].id, (unsigned)RA_LB_RAM);
    memmove(lb_ram, lb_ram + 1, (--lb_n) * sizeof(lb_ram[0]));
  }
  lb_ram[lb_n++] = e;
  debugf("RA: leaderboard %u result %ld kept in RAM until the server has it", e.id, (long)e.score);
}

static void lb_take(void) {
  lb_taking = true;   // counted by ra_task_lboard_pending() from before the receive
  lb_take_one();
  lb_taking = false;
}

/* Handles one event of the queue set: an unlock goes to the card, a leaderboard
   result into RAM, the clock (or a plain wake-up) only wakes the task. Also called
   by ra_net_get() while a request runs. A card set com_task asked for again is read
   here, whatever the task was waiting for, except while fetch_set() has the
   server's reply on its way into the same buffer. When the card does not give it,
   the logged-in loop asks the server once more. */
static void on_event(QueueSetMemberHandle_t m) {
  if(m == unlocks)        ra_queue_take(account);
  else if(m == lb_queue)  lb_take();
  else if(m == clock_sem) xSemaphoreTake(clock_sem, 0);
  if(!set_in_flight && !ra_patch_set_again()) set_reread_failed = true;
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
    // the table's id is checked against the server's, a fallback identity gets its id here
    ra_patch_resolved((unsigned)response.game_id);
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

/* Counts a refusal of a queued unlock of another game and returns the count. A
   small table by id and hash. The count stays at the limit until the card has
   taken the line, parked or confirmed, then refusals_forget() frees the slot: a
   park that fails on the card leaves the line at the head and its count at the
   limit, so the next refusal parks it again at once. With more than RA_REFUSE_MEM
   such lines at once the table is full, and a line it does not know is then parked
   at its first refusal (the count returned is the limit) instead of being counted:
   the queue is FIFO, so an entry given up for it would come back only after every
   other line, after another eviction, and nothing would ever be parked. */
static unsigned refusals(const ra_unlock_t *u) {
  unsigned i, free_slot = RA_REFUSE_MEM;
  for(i = 0; i < RA_REFUSE_MEM; i++) {
    if(!refused[i].n) { if(free_slot == RA_REFUSE_MEM) free_slot = i; continue; }
    if(refused[i].id == u->id && !strcmp(refused[i].hash, u->hash)) {
      if(refused[i].n < RA_REFUSE_MAX) refused[i].n++;   // saturates, see refusals_forget()
      return refused[i].n;
    }
  }
  if(free_slot == RA_REFUSE_MEM) {
    debugf("RA: more than %u refused unlocks of other games at once, unlock %u is parked at its first refusal",
           (unsigned)RA_REFUSE_MEM, u->id);
    return RA_REFUSE_MAX;
  }
  refused[free_slot].id = u->id;
  snprintf(refused[free_slot].hash, sizeof(refused[free_slot].hash), "%s", u->hash);
  refused[free_slot].n = 1;
  return 1;
}

/* Frees the count of a line once the card has taken it: parked, or confirmed by
   the server after all. Nothing to do for a line the table does not hold. */
static void refusals_forget(const ra_unlock_t *u) {
  unsigned i;
  for(i = 0; i < RA_REFUSE_MEM; i++)
    if(refused[i].n && refused[i].id == u->id && !strcmp(refused[i].hash, u->hash)) { refused[i].n = 0; return; }
}

/* Sends one unlock with r=awardachievement, under the hash it was earned with.
   An unlock of the running game is parked only when the server refuses it in a
   normal reply and the id is no longer in the active set, so a bad token or a
   server error never costs a real unlock. One of another game cannot be judged
   against this game's set: it moves behind the others until that game is booted
   next, and is parked after RA_REFUSE_MAX refusals so its backoff cannot hold up
   the running game's unlocks for good. "User already has" counts as done. */
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
  params.game_hash      = u->hash;
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
    bool own = ra_queue_own(u->hash);
    // the set belongs to com_task, it is only read here
    if(own && ra_patch_count() && !ra_patch_index(u->id)) {
      debugf("RA: unlock %u refused (%s), not in the set any more, parked", u->id, why);
      result = SUBMIT_PARK;
    } else if(!own && refusals(u) >= RA_REFUSE_MAX) {
      debugf("RA: unlock %u of game %s refused (%s), parked", u->id, u->hash, why);
      result = SUBMIT_PARK;
    } else {
      debugf("RA: unlock %u refused (%s), moved behind the others", u->id, why);
      result = SUBMIT_REQUEUE;
    }
  }
  rc_api_destroy_award_achievement_response(&response);
  return result;
}

/* Submits a leaderboard result with r=submitlbentry. The request has no mode, the
   server keeps an entry as a hardcore one and only from a client it has approved,
   which is why com_task hands over only hardcore results. A refusal in a normal
   reply drops it, as rc_client does. */
static submit_t submit_lboard(const lb_entry_t *e) {
  static rc_api_submit_lboard_entry_response_t response;   // about 300 bytes, the stack is for TLS
  rc_api_submit_lboard_entry_request_t params;
  rc_api_request_t request;
  rc_api_server_response_t server;
  ra_reply_t reply;
  submit_t result;
  uint32_t age = (uint32_t)((xTaskGetTickCount() - e->tick) / configTICK_RATE_HZ);

  if(age > RA_LB_MAX_AGE) {
    debugf("RA: leaderboard %u result older than 14 days, dropped", e->id);
    return SUBMIT_PARK;
  }
  memset(&params, 0, sizeof(params));
  params.username                 = user;
  params.api_token                = token;
  params.leaderboard_id           = e->id;
  params.score                    = e->score;
  params.game_hash                = ra_game_hash();
  params.seconds_since_completion = age;
  if(rc_api_init_submit_lboard_entry_request(&request, &params) != RC_OK) return SUBMIT_RETRY;
  int n = snprintf(path_buf, sizeof(path_buf), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);
  if(n < 0 || n >= (int)sizeof(path_buf)) return SUBMIT_RETRY;

  if(ra_net_get(path_buf, reply_buf, sizeof(reply_buf), &reply) != 0 ||
     reply.result != 0 || reply.status != 200)
    return SUBMIT_RETRY;
  if(reply.truncated) {
    debugf("RA: leaderboard %u: submitted, reply cut short", e->id);
    return SUBMIT_DONE;
  }
  server_reply(&server, &reply);
  memset(&response, 0, sizeof(response));
  int rv = rc_api_process_submit_lboard_entry_server_response(&response, &server);
  if(rv == RC_OK && response.response.succeeded) {
    debugf("RA: leaderboard %u: %ld submitted, best %ld, rank %u of %u", e->id, (long)response.submitted_score,
           (long)response.best_score, (unsigned)response.new_rank, (unsigned)response.num_entries);
    taskENTER_CRITICAL();
    lb_result.id      = e->id;
    lb_result.score   = e->score;
    lb_result.best    = response.best_score;
    lb_result.rank    = response.new_rank;
    lb_result.entries = response.num_entries;
    lb_seq++;
    taskEXIT_CRITICAL();
    result = SUBMIT_DONE;
  } else if(rv == RC_OK) {
    debugf("RA: leaderboard %u refused (%s), dropped", e->id,
           response.response.error_message ? response.response.error_message : "no reason given");
    result = SUBMIT_PARK;
  } else
    result = SUBMIT_RETRY;
  rc_api_destroy_submit_lboard_entry_response(&response);
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
  bool ok;

  memset(&params, 0, sizeof(params));
  params.username  = user;
  params.api_token = token;
  params.game_id   = ra_game_id();
  if(rc_api_init_fetch_game_data_request(&request, &params) != RC_OK) return false;
  snprintf(path, sizeof(path), "/dorequest.php?%s", request.post_data);
  rc_api_destroy_request(&request);

  // the buffer is taken from here on: a card set asked for again waits, see on_event()
  set_in_flight = true;
  if(ra_net_get_set(path, buf, cap, &reply) != 0 || reply.result != 0 || reply.status != 200 ||
     reply.broken)
    ok = false;
  else if(reply.truncated) {
    debugf("RA: set from the server is larger than %u bytes, not used", cap - 1);
    ok = true;
  } else
    ok = ra_patch_from_server(reply.len) >= 0;
  set_in_flight = false;
  return ok;
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
  // the device key for the tags on the card: without one, no hardcore
  switch(ra_mac_state()) {
  case RA_MAC_CREATED:  debugf("RA: device key created"); break;
  case RA_MAC_PRESENT:  debugf("RA: device key present"); break;
  case RA_MAC_REPLACED: debugf("RA: device key made anew, its flash sector held other data. "
                               "Unlocks tagged with an earlier key are set aside"); break;
  default:
    debugf("RA: no device key (%s), softcore only", ra_mac_error());
    ra_task_hardcore_block(RA_HC_BLOCK_KEY, true);
  }

  // 1. the set from the card: com_task has read it in ra_patch_settle() before
  //    this task was started, see main.c. The achievements run without an account
  //    too, only nothing goes to the server then.

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
  // the state before the queue, its lines count as unlocked too. A fallback
  // identity (the server resolves the id) has no state file yet: its queue is
  // opened now, so the owner exists before an unlock can be written, and scanned
  // again after the state is loaded, see step 4.
  bool fallback = ra_game_id() == 0;
  if(!fallback) ra_state_load(user);
  ra_queue_open(user);

  // nothing to talk to the server about: no ROM on an unknown board, or a game
  // that does not belong to this board. Queued lines of other games stay on the card.
  if(!ra_game_hash()[0]) {
    debugf("RA: no game, nothing to ask the server");
    state = RA_TASK_NO_GAME;
    sleep_forever();
  }

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
  // a fallback identity has its id now, or none: the server does not know the
  // hash, or it names a game of another board. With an id the state file can be
  // read, and the queue is scanned once more because ra_state_load replaces the
  // lists and would drop what the first scan counted as unlocked.
  if(fallback) {
    if(!ra_game_id()) {
      state = RA_TASK_NO_GAME;
      sleep_forever();
    }
    ra_state_load(user);
    ra_queue_open(user);
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
    // a card set asked for again (the boot's ROM is back after another game's
    // file) was not on the card, or had no valid tag: the server's set once more,
    // ra_patch_from_server() writes and hands it over anew
    if(set_reread_failed) { set_reread_failed = false; set_due = true; retry_set = false; }
    if(state_due) {
      state_due = false;
      state_failed = !fetch_state();
      if(!state_failed) backoff = RA_BACKOFF_MIN;
    }
    if(set_due) {
      set_due = false;
      set_failed = !fetch_set();
      if(!set_failed) backoff = RA_BACKOFF_MIN;
      // a card set asked for while the fetch had the buffer; not there: once more
      if(!ra_patch_set_again()) { set_due = true; retry_set = false; }
    }
    // both fetched, by whatever path: a retry time from an earlier failure is void
    if(!state_failed && !set_failed) retry_set = false;
    // leaderboard results first: they live in RAM only, the unlocks are on the card
    if(lb_n) {
      submit_t r = submit_lboard(&lb_ram[0]);
      if(r != SUBMIT_RETRY) {
        memmove(lb_ram, lb_ram + 1, (--lb_n) * sizeof(lb_ram[0]));
        backoff = RA_BACKOFF_MIN;
        continue;
      }
      wait_to_retry(&backoff, "leaderboard result not submitted");
      continue;
    }
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
      if(!ra_queue_own(u.hash)) refusals_forget(&u);   // the card took the line, its count is free
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
  // static storage: the FreeRTOS heap is nearly full once FTP runs
  lb_queue  = xQueueCreateStatic(RA_LB_QUEUE, sizeof(lb_entry_t), lb_queue_mem, &lb_queue_ctl);
  if(!events || !clock_sem || !unlocks || !lb_queue || !ra_patch_init() ||
     xQueueAddToSet(unlocks, events) != pdPASS || xQueueAddToSet(clock_sem, events) != pdPASS ||
     xQueueAddToSet(lb_queue, events) != pdPASS || !ra_net_init(events, on_event)) {
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
