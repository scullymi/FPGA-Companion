/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_slim.c
 *  @brief Takes the chunked framing off a set while it arrives and leaves out the fields
 *         nothing here reads.
 *
 *  A set from r=patch carries for every achievement two badge addresses and two rarity
 *  values. They make up about a quarter of a large set, and the set has to fit into
 *  ra_patch's buffer whole. lwIP's HTTP client hands over the
 *  reply in pieces, framing included, so both steps run byte by byte: the framing first,
 *  then the JSON. A pair that is left out is cut back out of the buffer once its key is
 *  known, together with the comma before it, or, first in its object, the comma after it. */
#include <string.h>

#include "ra_slim.h"

/* The fields of r=patch that nothing here reads: rcheevos builds the badge addresses
   from BadgeName when they are missing, and takes the rarities and the game's icon
   address as optional (rc_api_runtime.c). Each holds a string or a number. */
static const char *const unused[] = {
  "BadgeURL", "BadgeLockedURL", "Rarity", "RarityHardcore", "ImageIconURL"
};

enum { FRAME_START, FRAME_PLAIN, FRAME_LEN, FRAME_EXT, FRAME_DATA, FRAME_DATA_END, FRAME_DONE };
enum { JSON_OUT, JSON_STR, JSON_STR_ESC, JSON_SKIP, JSON_SKIP_STR, JSON_SKIP_STR_ESC,
       JSON_SKIP_NUM, JSON_SKIP_END };

void ra_slim_init(ra_slim_t *s) {
  memset(s, 0, sizeof(*s));
}

void ra_slim_framing(ra_slim_t *s, bool chunked) {
  s->frame = chunked ? FRAME_LEN : FRAME_PLAIN;
}

static int hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* No NUL after each byte: in place it would overwrite the next byte of the input,
   ra_slim_feed() ends the buffer once per call. */
static bool put(char c, char *buf, unsigned cap, unsigned *len) {
  if(*len + 1 >= cap) return false;
  buf[(*len)++] = c;
  return true;
}

static bool unused_key(const char *key, unsigned n) {
  unsigned i;
  for(i = 0; i < sizeof(unused) / sizeof(unused[0]); i++)
    if(strlen(unused[i]) == n && !memcmp(unused[i], key, n)) return true;
  return false;
}

/* One byte of the JSON, the framing is off already. false when the buffer is full. */
static bool json(ra_slim_t *s, char c, char *buf, unsigned cap, unsigned *len) {
  switch(s->json) {
  case JSON_STR:
    if(c == '\\') s->json = JSON_STR_ESC;
    else if(c == '"') {
      s->json = JSON_OUT;
      s->str_end = *len;
      s->sig_at = *len;
      s->after_str = true;
    }
    return put(c, buf, cap, len);
  case JSON_STR_ESC:
    s->json = JSON_STR;
    return put(c, buf, cap, len);
  // the value of a pair that is left out, up to the comma or bracket after it
  case JSON_SKIP:
    s->dropped++;
    if(space(c)) return true;
    if(c == '"') s->json = JSON_SKIP_STR;
    else if(c == '{' || c == '[') s->broken = true;   // only strings and numbers are left out
    else s->json = JSON_SKIP_NUM;
    return true;
  case JSON_SKIP_STR:
    s->dropped++;
    if(c == '\\') s->json = JSON_SKIP_STR_ESC;
    else if(c == '"') s->json = JSON_SKIP_END;
    return true;
  case JSON_SKIP_STR_ESC:
    s->dropped++;
    s->json = JSON_SKIP_STR;
    return true;
  case JSON_SKIP_NUM:
    if(c != ',' && c != '}' && c != ']' && !space(c)) { s->dropped++; return true; }
    s->json = JSON_SKIP_END;
    __attribute__((fallthrough));      // c ends the number
  case JSON_SKIP_END:
    if(space(c)) { s->dropped++; return true; }
    s->json = JSON_OUT;
    if(c == ',' && s->drop_comma) {
      s->drop_comma = false;
      s->dropped++;
      return true;
    }
    s->drop_comma = false;
    break;                              // c is a bracket or a comma that stays
  }

  // outside strings
  if(c == '"') {
    s->before_str = s->sig_at;
    s->str_at = *len;
    s->after_str = false;
    s->json = JSON_STR;
    return put(c, buf, cap, len);
  }
  if(c == ':' && s->after_str &&
     unused_key(buf + s->str_at + 1, s->str_end - s->str_at - 1)) {
    // the key is in the buffer already: cut it back out, with the comma before it, or
    // with the comma after its value when the pair is the first of its object
    unsigned cut = s->str_at;
    if(s->str_at > 0 && buf[s->before_str] == ',') cut = s->before_str;
    else s->drop_comma = true;
    s->dropped += *len - cut + 1;       // the colon too
    *len = cut;
    buf[cut] = 0;
    s->sig_at = s->before_str;
    s->after_str = false;
    s->json = JSON_SKIP;
    return true;
  }
  if(!space(c)) {
    s->after_str = false;
    s->sig_at = *len;
  }
  return put(c, buf, cap, len);
}

bool ra_slim_feed(ra_slim_t *s, const char *in, unsigned n, char *buf, unsigned cap, unsigned *len) {
  unsigned i;
  for(i = 0; i < n && !s->broken; i++) {
    char c = in[i];
    int v;
    // a file has no header: plain JSON starts with a bracket, chunked framing with a hex length
    if(s->frame == FRAME_START) s->frame = (c == '{' || c == '[') ? FRAME_PLAIN : FRAME_LEN;
    switch(s->frame) {
    case FRAME_PLAIN:
      if(!json(s, c, buf, cap, len)) { buf[*len] = 0; return false; }
      break;
    // a chunk: its length in hex, maybe extensions, CR LF, the data, CR LF. Length 0 ends it.
    case FRAME_LEN:
      v = hexval(c);
      if(v >= 0) {
        if(s->left > 0x0FFFFFFFUL) s->broken = true;   // no chunk is that long
        s->left = s->left * 16 + (unsigned long)v;
        s->digits = true;
      } else if(!s->digits) s->broken = true;
      else if(c == '\n') s->frame = s->left ? FRAME_DATA : FRAME_DONE;
      else s->frame = FRAME_EXT;
      break;
    case FRAME_EXT:
      if(c == '\n') s->frame = s->left ? FRAME_DATA : FRAME_DONE;
      break;
    case FRAME_DATA:
      if(!json(s, c, buf, cap, len)) { buf[*len] = 0; return false; }
      if(--s->left == 0) s->frame = FRAME_DATA_END;
      break;
    case FRAME_DATA_END:
      if(c == '\n') {
        s->frame = FRAME_LEN;
        s->digits = false;
      } else if(c != '\r') s->broken = true;
      break;
    default:                            // FRAME_DONE: trailers, unused
      break;
    }
  }
  buf[*len] = 0;
  return true;
}

bool ra_slim_whole(const ra_slim_t *s) {
  if(s->broken || s->json != JSON_OUT) return false;
  return s->frame == FRAME_PLAIN || s->frame == FRAME_DONE;
}
