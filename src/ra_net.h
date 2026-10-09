/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_net.h
 *  @brief HTTPS GET to retroachievements.org, see ra_net.c. */
#ifndef RA_NET_H
#define RA_NET_H

#include <stdbool.h>
#include <FreeRTOS.h>
#include <queue.h>

/** @brief What came back from a request. */
typedef struct {
  int           result;      /**< httpc_result_t, 0 = HTTPC_RESULT_OK */
  int           err;         /**< lwIP error of a failed request */
  unsigned long status;      /**< HTTP status, 0 when none arrived */
  unsigned      len;         /**< bytes in the buffer, which is NUL-terminated */
  bool          truncated;   /**< the reply did not fit into the buffer */
  bool          broken;      /**< the body was cut short or its framing not as expected */
} ra_reply_t;

#define RA_NET_NO_LENGTH 0xFFFFFFFFUL   /**< ra_net_framing_t.length without a Content-Length */

/** @brief How the header frames the body (RFC 9112, 6.3), see ra_net_framing_init(). */
typedef struct {
  char          line[48];    /**< the current header line without CR, cut at its size */
  unsigned      at;          /**< bytes of the current line, those cut included */
  bool          coded;       /**< a Transfer-Encoding came, Content-Length does not count */
  bool          chunked;     /**< chunked is its last coding */
  bool          bad;         /**< a framing field that cannot be read */
  unsigned long length;      /**< Content-Length, RA_NET_NO_LENGTH without one */
  unsigned long got;         /**< body bytes received */
} ra_net_framing_t;

/** @brief Starts a header: no Transfer-Encoding, no Content-Length. */
void ra_net_framing_init(ra_net_framing_t *f);

/** @brief Reads n more bytes of the header, status line included, in pieces of any size. */
void ra_net_framing_feed(ra_net_framing_t *f, const char *in, unsigned n);

/** @brief The header could be read and the body came as long as it said. A coded body
 *         ends with the connection, its chunked framing is checked where it comes off.
 *         Otherwise Content-Length counts, without one the end of the connection. */
bool ra_net_framing_whole(const ra_net_framing_t *f);

/** @brief Joins the RA task's queue set. Once, before the first request.
 *
 *  The end of a request then arrives through that set. While ra_net_get() waits
 *  for it, every other event of the set goes to on_event, so the task goes on
 *  handling them. false when the set could not be joined. */
bool ra_net_init(QueueSetHandle_t set, void (*on_event)(QueueSetMemberHandle_t member));

/** @brief GET path from https://retroachievements.org into buf and wait for the end.
 *
 *  Only from the RA task, one request at a time. Returns 0 when the request ran
 *  (see reply->result and ->status), -1 when it could not start. The log shows
 *  only the kind of request (r=...), never the query, which may carry the token. */
int ra_net_get(const char *path, char *buf, unsigned cap, ra_reply_t *reply);

/** @brief ra_net_get() for a set (r=patch): the framing comes off and the fields nothing
 *         reads stay out while the reply arrives (ra_slim.c), so a larger set fits into buf. */
int ra_net_get_set(const char *path, char *buf, unsigned cap, ra_reply_t *reply);

/** @brief Removes the chunked transfer framing from a body, in place.
 *
 *  lwIP's HTTP client passes it through. Only for a body whose header says chunked.
 *  len is updated and the body stays NUL-terminated. false, and the body as it
 *  came, when it is not whole chunked framing. */
bool ra_net_dechunk(char *buf, unsigned *len);

/** @brief The User-Agent sent with every request, e.g. "game20k/v0.1.0 (Tang Nano 20K) rcheevos/12.5". */
const char *ra_user_agent(void);

#endif /* RA_NET_H */
