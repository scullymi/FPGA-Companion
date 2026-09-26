/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_net.h
 *  @brief HTTPS GET to retroachievements.org, see ra_net.c. */
#ifndef RA_NET_H
#define RA_NET_H

#include <stdbool.h>

/** @brief What came back from a request. */
typedef struct {
  int           result;      /**< httpc_result_t, 0 = HTTPC_RESULT_OK */
  int           err;         /**< lwIP error of a failed request */
  unsigned long status;      /**< HTTP status, 0 when none arrived */
  unsigned      len;         /**< bytes in the buffer, which is NUL-terminated */
  bool          truncated;   /**< the reply did not fit into the buffer */
} ra_reply_t;

/** @brief GET path from https://retroachievements.org into buf and wait for the end.
 *
 *  Only from the RA task, one request at a time. Returns 0 when the request ran
 *  (see reply->result and ->status), -1 when it could not start. The log shows
 *  only the kind of request (r=...), never the query, which may carry the token. */
int ra_net_get(const char *path, char *buf, unsigned cap, ra_reply_t *reply);

/** @brief The User-Agent sent with every request, e.g. "game20k/v0.1.0 (Tang Nano 20K) rcheevos/12.5". */
const char *ra_user_agent(void);

#endif /* RA_NET_H */
