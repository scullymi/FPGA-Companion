/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_slim.h
 *  @brief Takes the chunked framing off a reply while it arrives, leaves out the fields of a
 *         set nothing here reads, and reads how a reply's header frames its body, see ra_slim.c. */
#ifndef RA_SLIM_H
#define RA_SLIM_H

#include <stdbool.h>

/** @brief The state between two pieces of a reply. Zeroed by ra_slim_init(). */
typedef struct {
  unsigned char frame;      /**< where in the chunked framing, FRAME_* in ra_slim.c */
  unsigned char json;       /**< where in the JSON, JSON_* in ra_slim.c */
  unsigned long left;       /**< bytes left in the current chunk, or the length being read */
  bool          digits;     /**< the length line has a digit */
  bool          after_str;  /**< the last token was a string, a ':' makes it a key */
  bool          drop_comma; /**< a pair was left out first in its object, the next comma goes too */
  bool          broken;     /**< framing or JSON not as expected, the reply is unusable */
  unsigned      str_at;     /**< buffer offset of the opening quote of the last string */
  unsigned      str_end;    /**< buffer offset of its closing quote */
  unsigned      sig_at;     /**< buffer offset of the last character outside strings that is not space */
  unsigned      before_str; /**< sig_at when the last string began */
  unsigned long dropped;    /**< bytes left out, for the log */
  bool          keep;       /**< every field stays, only the framing comes off: a reply that is no set */
} ra_slim_t;

/** @brief Starts a reply. */
void ra_slim_init(ra_slim_t *s);

/** @brief The reply's header says whether the body is chunked. Before the first
 *         ra_slim_feed(). Without it, for a set from the card, the first byte decides. */
void ra_slim_framing(ra_slim_t *s, bool chunked);

/** @brief Takes n more bytes of the reply as they came, writes the slim JSON to buf at *len.
 *
 *  buf holds cap bytes and is NUL-terminated on return. false when the result does not
 *  fit, *len then holds what did, and the rest of the reply is not wanted. in may be buf
 *  itself, in one call with *len 0: the output never overtakes the input. */
bool ra_slim_feed(ra_slim_t *s, const char *in, unsigned n, char *buf, unsigned cap, unsigned *len);

/** @brief The reply is whole: a plain body, or chunked framing up to its last chunk, and
 *         nothing was broken. */
bool ra_slim_whole(const ra_slim_t *s);

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

#endif /* RA_SLIM_H */
