/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file games_file.c
 *  @brief The footer of a ROM file.
 *
 *  scripts/make_rom.py ends every ROM file with a footer of GAMES_FOOTER_SIZE
 *  bytes, little-endian:
 *
 *     0  4  "G20K"                 76 32  SHA-256 of the content
 *     4  1  version (1)           108 16  zero
 *     5  1  board id              124  4  CRC-32 of bytes 0 to 123
 *     6  1  screen code
 *     7  1  zero
 *     8  4  size of the content
 *    12 16  set name, ASCII, NUL-padded
 *    28 48  title, ASCII, NUL-padded
 *
 *  Only the content goes to the core, and the SHA-256 of what streamed is compared
 *  with the footer's when the stream ends (sdc.c).
 *
 *  No task state lives here: the file links into a host test, and the only FatFs
 *  use is games_footer_read(). */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "games_file.h"

uint32_t games_crc32(const void *data, size_t len) {
  const unsigned char *p = data;
  uint32_t crc = 0xFFFFFFFFu;
  // bit by bit: 124 bytes per footer, a table would cost 1 KB of flash for nothing
  while(len--) {
    crc ^= *p++;
    for(int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
  }
  return ~crc;
}

static uint32_t le32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A NUL-padded text field into a string: at most n characters, the rest of the
   field is not looked at. */
static void field(char *out, const unsigned char *p, size_t n) {
  size_t i;
  for(i = 0; i < n && p[i]; i++) out[i] = (char)p[i];
  out[i] = 0;
}

bool games_footer_parse(const unsigned char *buf, FSIZE_t file_size, games_footer_t *out) {
  if(file_size < GAMES_FOOTER_SIZE) return false;
  if(memcmp(buf, "G20K", 4) || buf[4] != GAMES_FOOTER_VERSION) return false;
  if(games_crc32(buf, 124) != le32(buf + 124)) return false;
  // the content ends where the footer starts, else the file was cut or extended
  if((FSIZE_t)le32(buf + 8) != file_size - GAMES_FOOTER_SIZE) return false;
  out->board   = buf[5];
  out->screen  = buf[6];
  out->content = le32(buf + 8);
  field(out->set, buf + 12, GAMES_SET_MAX);
  field(out->title, buf + 28, GAMES_TITLE_MAX);
  memcpy(out->sha, buf + 76, 32);
  return true;
}

bool games_footer_read(FIL *f, games_footer_t *out) {
  unsigned char buf[GAMES_FOOTER_SIZE];
  FSIZE_t size = f_size(f), pos = f_tell(f);
  UINT n = 0;
  bool ok;
  if(size < GAMES_FOOTER_SIZE) return false;
  ok = f_lseek(f, size - GAMES_FOOTER_SIZE) == FR_OK && f_read(f, buf, sizeof(buf), &n) == FR_OK &&
       n == sizeof(buf) && games_footer_parse(buf, size, out);
  if(f_lseek(f, pos) != FR_OK) ok = false;
  return ok;
}

int games_order(const char *title_a, const char *name_a, const char *title_b, const char *name_b) {
  int r = strcasecmp(title_a, title_b);
  return r ? r : strcmp(name_a, name_b);
}
