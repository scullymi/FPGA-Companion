/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_slim.h
 *  @brief Takes the chunked framing off a set while it arrives and leaves out the fields
 *         nothing here reads, see ra_slim.c. */
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
} ra_slim_t;

/** @brief Starts a reply. */
void ra_slim_init(ra_slim_t *s);

/** @brief Takes n more bytes of the reply as they came, writes the slim JSON to buf at *len.
 *
 *  buf holds cap bytes and is NUL-terminated on return. false when the result does not
 *  fit, *len then holds what did, and the rest of the reply is not wanted. in may be buf
 *  itself, in one call with *len 0: the output never overtakes the input. */
bool ra_slim_feed(ra_slim_t *s, const char *in, unsigned n, char *buf, unsigned cap, unsigned *len);

/** @brief The reply is whole: plain JSON, or chunked framing up to its last chunk, and
 *         nothing was broken. */
bool ra_slim_whole(const ra_slim_t *s);

#endif /* RA_SLIM_H */
