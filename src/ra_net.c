/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_net.c
 *  @brief HTTPS GET to retroachievements.org over lwIP's HTTP client and mbedTLS.
 *
 *  Encrypted because the connect token travels in the query. The server
 *  certificate is checked against ra_ca.h, and a failed check ends the
 *  connection (ALTCP_MBEDTLS_AUTHMODE in lwipopts.h). */
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include "lwip/tcpip.h"
#include "lwip/apps/http_client.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/platform_util.h"
#include "pico/time.h"

#include "rc_version.h"

#include "debug.h"
// the SDK heap left and the FreeRTOS heap left, for the log. Weak stand-ins that give 0:
// the firmware's mcu_hw.c and FreeRTOS define the real ones, the host test links neither
__attribute__((weak)) uint32_t getFreeHeap(void) { return 0; }
__attribute__((weak)) size_t xPortGetFreeHeapSize(void) { return 0; }
#include "ra_ca.h"
#include "ra_net.h"
#include "ra_slim.h"

#define RA_HOST "retroachievements.org"   /**< the server, and the name its certificate must carry */

static struct altcp_tls_config *tls;
static altcp_allocator_t        allocator;
static httpc_connection_t       conn;
static httpc_state_t           *req;
static SemaphoreHandle_t        done;      /* given by on_done, a member of the RA task's queue set */
static volatile bool            busy;      /* set at the start, cleared by on_done or when the request cannot start */
static QueueSetHandle_t         events;    /* the RA task's queue set */
static void (*on_event)(QueueSetMemberHandle_t);   /* its handler for everything but done */

static char       *dst;
static unsigned    dst_cap, dst_len;
static ra_reply_t *out;
static bool        slim_on;   /* the reply is a set: framing off and unused fields out while it arrives */
static ra_slim_t   slim;
static ra_net_framing_t framing;   /* how the reply's header frames its body */

/* RetroAchievements knows a client by its User-Agent, so it names this firmware
   truthfully and never another emulator. Version and platform come from the
   build (CMakeLists.txt), the rcheevos version from rcheevos itself.
   example: "game20k/v0.1.1 (Tang Nano 20K) rcheevos/12.5" */
const char *ra_user_agent(void) {
  return "game20k/v" GAME20K_VERSION " (" GAME20K_PLATFORM ") rcheevos/" RCHEEVOS_VERSION_STRING;
}

/** @brief Milliseconds since boot, the clock mbedTLS asks for (MBEDTLS_PLATFORM_MS_TIME_ALT). */
mbedtls_ms_time_t mbedtls_ms_time(void) {
  return (mbedtls_ms_time_t)(to_us_since_boot(get_absolute_time()) / 1000);
}

/* lwIP's HTTP client does not set the server name. The server needs it (SNI),
   and mbedTLS checks the certificate against it. Without a name mbedTLS would
   skip that check, so no connection is opened without it. */
static struct altcp_pcb *alloc_sni(void *arg, u8_t ip_type) {
  struct altcp_pcb *pcb = altcp_tls_alloc(arg, ip_type);
  if(!pcb) return NULL;
  mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
  if(!ssl || mbedtls_ssl_set_hostname(ssl, RA_HOST) != 0) {
    altcp_abort(pcb);       // the request then fails at its start
    return NULL;
  }
  return pcb;
}

static err_t on_recv(__attribute__((unused)) void *arg, struct altcp_pcb *pcb,
                     struct pbuf *p, __attribute__((unused)) err_t err) {
  if(!p) return ERR_OK;
  framing.got += p->tot_len;
  if(slim_on) {
    // piece by piece through ra_slim, nothing more once the buffer is full
    for(struct pbuf *q = p; q && !out->truncated; q = q->next)
      if(!ra_slim_feed(&slim, (const char *)q->payload, q->len, dst, dst_cap, &dst_len))
        out->truncated = true;
  } else {
    unsigned room = dst_cap - 1 - dst_len;
    unsigned take = p->tot_len < room ? p->tot_len : room;
    if(take < p->tot_len) out->truncated = true;
    pbuf_copy_partial(p, dst + dst_len, take, 0);
    dst_len += take;
    dst[dst_len] = 0;
  }
  altcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

/* lwIP hands over the whole header before the body, also when it came in several pbufs.
   The pbufs may carry the first body bytes after hdr_len. */
static err_t on_headers(__attribute__((unused)) httpc_state_t *c, __attribute__((unused)) void *arg,
                        struct pbuf *hdr, u16_t hdr_len, __attribute__((unused)) u32_t content_len) {
  for(struct pbuf *q = hdr; q && hdr_len; q = q->next) {
    u16_t n = q->len < hdr_len ? q->len : hdr_len;
    ra_net_framing_feed(&framing, (const char *)q->payload, n);
    hdr_len -= n;
  }
  if(slim_on) ra_slim_framing(&slim, framing.chunked);
  return ERR_OK;
}

static void on_done(__attribute__((unused)) void *arg, httpc_result_t result,
                    __attribute__((unused)) u32_t len, u32_t status, err_t err) {
  out->result = (int)result;
  out->err    = (int)err;
  out->status = status;
  out->len    = dst_len;
  req  = NULL;
  busy = false;
  xSemaphoreGive(done);
}

static int hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void ra_net_framing_init(ra_net_framing_t *f) {
  memset(f, 0, sizeof(*f));
  f->length = RA_NET_NO_LENGTH;
}

/* A header field by name, case ignored, and its value without the space around it. */
static bool field(const char *line, unsigned n, const char *name, const char **v, unsigned *vn) {
  unsigned k = (unsigned)strlen(name);
  if(n <= k || line[k] != ':' || strncasecmp(line, name, k)) return false;
  *v  = line + k + 1;
  *vn = n - k - 1;
  while(*vn && (**v == ' ' || **v == '\t')) { (*v)++; (*vn)--; }
  while(*vn && ((*v)[*vn - 1] == ' ' || (*v)[*vn - 1] == '\t')) (*vn)--;
  return true;
}

/* One header line. Only the two fields that frame the body count, the status line
   and all others pass. */
static void framing_line(ra_net_framing_t *f) {
  bool cut = f->at > sizeof(f->line);
  unsigned n = cut ? sizeof(f->line) : f->at, vn, i;
  unsigned long len = 0;
  const char *v;

  if(field(f->line, n, "Transfer-Encoding", &v, &vn)) {
    // the codings in the order they were applied, chunked has to be the last
    i = vn;
    while(i && v[i - 1] != ',') i--;
    while(i < vn && (v[i] == ' ' || v[i] == '\t')) i++;
    f->coded   = true;
    f->chunked = vn - i == 7 && !strncasecmp(v + i, "chunked", 7);
  } else if(field(f->line, n, "Content-Length", &v, &vn)) {
    for(i = 0; i < vn && v[i] >= '0' && v[i] <= '9' && len < 0x0FFFFFFFUL; i++)
      len = len * 10 + (unsigned long)(v[i] - '0');
    // digits only, and a second Content-Length the same as the first
    if(!vn || i < vn || (f->length != RA_NET_NO_LENGTH && f->length != len)) f->bad = true;
    f->length = len;
  } else
    return;
  if(cut) f->bad = true;    // the value is not all there
}

void ra_net_framing_feed(ra_net_framing_t *f, const char *in, unsigned n) {
  for(; n; in++, n--) {
    if(*in == '\n') {
      framing_line(f);
      f->at = 0;
    } else if(*in != '\r') {
      if(f->at < sizeof(f->line)) f->line[f->at] = *in;
      f->at++;
    }
  }
}

bool ra_net_framing_whole(const ra_net_framing_t *f) {
  return !f->bad && (f->coded || f->length == RA_NET_NO_LENGTH || f->got == f->length);
}

bool ra_net_dechunk(char *buf, unsigned *len) {
  char *end = buf + *len;
  int pass;

  if(*len == 0) return false;
  // each chunk is a hex length, CR LF, the data, CR LF, and a length of 0 ends it.
  // Two passes: the first only checks that the framing is whole, the second moves
  // the data down. A body that is not whole chunked framing stays as it came.
  for(pass = 0; pass < 2; pass++) {
    char *in = buf, *out = buf;
    for(;;) {
      unsigned long n = 0;
      char *p = in;
      while(p < end && hexval(*p) >= 0) { n = n * 16 + (unsigned long)hexval(*p); p++; }
      if(p == in) return false;                       // no length line: not chunked
      while(p < end && *p != '\n') p++;               // chunk extensions and the CR
      if(p >= end) return false;
      p++;                                            // the LF
      if(n == 0) break;                               // the last chunk
      if((unsigned long)(end - p) < n) return false;  // cut short
      if(pass) memmove(out, p, n);
      out += n;
      in = p + n;
      if(in < end && *in == '\r') in++;
      if(in < end && *in == '\n') in++;
    }
    if(pass) {
      *out = 0;
      *len = (unsigned)(out - buf);
    }
  }
  return true;
}

/* the kind of request, the r= parameter, for the log */
static void kind_of(const char *path, char *kind, size_t size) {
  const char *r = strstr(path, "r=");
  size_t n = 0;
  if(r) {
    r += 2;
    while(r[n] && r[n] != '&' && n < size - 1) n++;
    memcpy(kind, r, n);
  }
  kind[n] = 0;
}

bool ra_net_init(QueueSetHandle_t set, void (*handler)(QueueSetMemberHandle_t)) {
  if(events) return true;
  if(!set || !handler) return false;
  if(!done && !(done = xSemaphoreCreateBinary())) return false;
  if(xQueueAddToSet(done, set) != pdPASS) return false;
  on_event = handler;
  events   = set;
  return true;
}

static int get(const char *path, char *buf, unsigned cap, ra_reply_t *reply, bool set) {
  char kind[24];                      // "awardachievement" is the longest
  err_t e = ERR_MEM;

  if(busy || !events || !buf || cap < 2 || !reply) return -1;

  memset(reply, 0, sizeof(*reply));
  dst = buf; dst_cap = cap; dst_len = 0; dst[0] = 0; out = reply;
  slim_on = set;
  ra_slim_init(&slim);
  ra_net_framing_init(&framing);
  kind_of(path, kind, sizeof(kind));
  absolute_time_t t0 = get_absolute_time();

  // lwIP's core lock, the same on WiFi and on USB Ethernet
  LOCK_TCPIP_CORE();
  // sizeof includes the final NUL, lwIP needs it to parse PEM
  if(!tls) tls = altcp_tls_create_config_client((const u8_t *)ra_ca_pem, sizeof(ra_ca_pem));
  if(tls) {
    allocator.alloc = alloc_sni;
    allocator.arg   = tls;
    memset(&conn, 0, sizeof(conn));
    conn.altcp_allocator = &allocator;
    conn.result_fn       = on_done;
    conn.headers_done_fn = on_headers;
    busy = true;
    e = httpc_get_file_dns(RA_HOST, 443, path, &conn, on_recv, NULL, &req);
    if(e != ERR_OK) busy = false;     // no callback follows then
  }
  UNLOCK_TCPIP_CORE();

  if(!tls) { debugf("RA: TLS setup failed"); return -1; }
  if(e != ERR_OK) { debugf("RA: r=%s could not start, error %d", kind, (int)e); return -1; }

  // lwIP always ends a request with on_done, its own timeouts included. Until
  // then the task's other events are handled here, none waits for the server.
  for(;;) {
    QueueSetMemberHandle_t m = xQueueSelectFromSet(events, pdMS_TO_TICKS(30000));
    if(m == done) { xSemaphoreTake(done, 0); break; }
    if(m) on_event(m);
    else  debugf("RA: r=%s still waiting for the server", kind);
  }

  unsigned long ms = (unsigned long)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
  // lwIP's HTTP client passes the chunked framing through. A whole reply loses it here
  // when its header says chunked, a set lost it in ra_slim already. A reply cut short
  // or not as its header framed it is unusable.
  if(reply->result == HTTPC_RESULT_OK && !reply->truncated) {
    bool whole = ra_net_framing_whole(&framing);
    if(set) whole = whole && ra_slim_whole(&slim);
    else if(whole && framing.chunked) whole = ra_net_dechunk(buf, &reply->len);
    reply->broken = !whole;
  }
  // with the SDK heap left after the connection: a set rcheevos holds and a large
  // reply both live there, a request that finds too little fails and is asked again
  if(reply->result == HTTPC_RESULT_OK) {
    debugf("RA: r=%s -> HTTP %lu, %u bytes%s%s%s, %lu ms, heap free %lu/%u", kind, reply->status, reply->len,
           framing.chunked ? " (chunked)" : "", reply->truncated ? " (truncated)" : "",
           reply->broken ? " (not whole)" : "", ms,
           (unsigned long)getFreeHeap(), (unsigned)xPortGetFreeHeapSize());
    if(set) debugf("RA: r=%s %lu bytes of unused fields left out", kind, slim.dropped);
  } else
    debugf("RA: r=%s failed, result %d, lwIP error %d, %lu ms", kind, reply->result, reply->err, ms);
  return 0;
}

int ra_net_get(const char *path, char *buf, unsigned cap, ra_reply_t *reply) {
  return get(path, buf, cap, reply, false);
}

int ra_net_get_set(const char *path, char *buf, unsigned cap, ra_reply_t *reply) {
  return get(path, buf, cap, reply, true);
}
