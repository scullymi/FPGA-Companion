/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_net.c
 *  @brief HTTPS GET to retroachievements.org over lwIP's HTTP client and mbedTLS.
 *
 *  Encrypted because the connect token travels in the query. The server
 *  certificate is checked against ra_ca.h, and a failed check ends the
 *  connection (ALTCP_MBEDTLS_AUTHMODE in lwipopts.h). */
#include <string.h>
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
#include "ra_ca.h"
#include "ra_net.h"

#define RA_HOST "retroachievements.org"   /**< the server, and the name its certificate must carry */

static struct altcp_tls_config *tls;
static altcp_allocator_t        allocator;
static httpc_connection_t       conn;
static httpc_state_t           *req;
static SemaphoreHandle_t        done;
static volatile bool            busy;      /* set at the start, cleared by on_done or when the request cannot start */

static char       *dst;
static unsigned    dst_cap, dst_len;
static ra_reply_t *out;

/* RetroAchievements knows a client by its User-Agent, so it names this firmware
   truthfully and never another emulator. Version and platform come from the
   build (CMakeLists.txt), the rcheevos version from rcheevos itself.
   example: "game20k/v1.0.0 (Raspberry Pi Pico) rcheevos/1.0.0" */
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
  unsigned room = dst_cap - 1 - dst_len;
  unsigned take = p->tot_len < room ? p->tot_len : room;
  if(take < p->tot_len) out->truncated = true;
  pbuf_copy_partial(p, dst + dst_len, take, 0);
  dst_len += take;
  dst[dst_len] = 0;
  altcp_recved(pcb, p->tot_len);
  pbuf_free(p);
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

int ra_net_get(const char *path, char *buf, unsigned cap, ra_reply_t *reply) {
  char kind[16];
  err_t e = ERR_MEM;

  if(busy || !buf || cap < 2 || !reply) return -1;
  if(!done && !(done = xSemaphoreCreateBinary())) return -1;

  memset(reply, 0, sizeof(*reply));
  dst = buf; dst_cap = cap; dst_len = 0; dst[0] = 0; out = reply;
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
    busy = true;
    e = httpc_get_file_dns(RA_HOST, 443, path, &conn, on_recv, NULL, &req);
    if(e != ERR_OK) busy = false;     // no callback follows then
  }
  UNLOCK_TCPIP_CORE();

  if(!tls) { debugf("RA: TLS setup failed"); return -1; }
  if(e != ERR_OK) { debugf("RA: r=%s could not start, error %d", kind, (int)e); return -1; }

  // lwIP always ends a request with on_done, its own timeouts included
  while(xSemaphoreTake(done, pdMS_TO_TICKS(30000)) != pdTRUE)
    debugf("RA: r=%s still waiting for the server", kind);

  unsigned long ms = (unsigned long)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
  if(reply->result == HTTPC_RESULT_OK)
    debugf("RA: r=%s -> HTTP %lu, %u bytes%s, %lu ms", kind, reply->status, reply->len,
           reply->truncated ? " (truncated)" : "", ms);
  else
    debugf("RA: r=%s failed, result %d, lwIP error %d, %lu ms", kind, reply->result, reply->err, ms);
  return 0;
}
