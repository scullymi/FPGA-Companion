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
  bool          broken;      /**< ra_net_get_set() only: the reply was cut short or not as expected */
} ra_reply_t;

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
 *  lwIP's HTTP client passes it through. len is updated and the body stays
 *  NUL-terminated. A body that starts with '{' is plain already and stays. false
 *  when the body is neither plain nor whole chunked framing. */
bool ra_net_dechunk(char *buf, unsigned *len);

/** @brief The User-Agent sent with every request, e.g. "game20k/v0.1.0 (Tang Nano 20K) rcheevos/12.5". */
const char *ra_user_agent(void);

#endif /* RA_NET_H */
