/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file stone.c
 *  @brief The stepping stone: a jump straight to a core in the configuration flash.
 *
 *  The stepping stone is an empty bitstream. Its header carries the 0xD2 command, the address
 *  the FPGA loads from at the next RECONFIG. 0xD2 lies outside the CRC, so the address is set
 *  while the bitstream goes out over JTAG, and a RECONFIG over JTAG right after the load jumps
 *  to the core in that slot. The slots come from the running core's scan of the flash
 *  (ram_mirror_slots). */
#include <string.h>

#include "../mcu_hw.h" /* ENABLE_JTAG */
#include "stone.h"

/* A board id of a slot: 0 is an empty slot, 255 a core without a tag. */
static bool is_board(unsigned char b) { return b != 0 && b != 255; }

int stone_slot(const unsigned char slots[RAM_MIRROR_SLOTS], bool known, unsigned char board) {
  if(!known || !is_board(board)) return STONE_NONE;
  for(int i = 0; i < RAM_MIRROR_SLOTS; i++)
    if(slots[i] == board) return i;
  return STONE_NONE;
}

int stone_hops(const unsigned char slots[RAM_MIRROR_SLOTS], bool known, unsigned char from,
               unsigned char to) {
  int a = stone_slot(slots, known, from), b = stone_slot(slots, known, to);
  if(a == STONE_NONE || b == STONE_NONE) return STONE_NONE;
  // the ring ends at the last slot that is not empty, also a core without a tag: its header
  // points back to the first slot
  int n = RAM_MIRROR_SLOTS;
  while(!slots[n - 1]) n--;
  return (b - a + n) % n;
}

void stone_patch(unsigned char *buf, size_t len, size_t pos, size_t offset, uint32_t address) {
  // the four address bytes follow d2 00 ff ff, most significant first
  for(size_t i = 0; i < 4; i++) {
    size_t at = offset + 4 + i;
    if(at >= pos && at < pos + len) buf[at - pos] = (unsigned char)(address >> (8 * (3 - i)));
  }
}

#ifdef ENABLE_JTAG
#include "../gowin.h"

/* The read function of stone_jump(): the next bytes of the stepping stone, with the jump address
   in its 0xD2 command. */
typedef struct {
  size_t pos;
  uint32_t address;
} stone_reader_t;

static bool stone_read(void *ctx, uint8_t *buf, uint16_t len) {
  stone_reader_t *r = ctx;
  memcpy(buf, stone_bits + r->pos, len);
  stone_patch(buf, len, r->pos, stone_d2, r->address);
  r->pos += len;
  return true;
}

bool stone_jump(int slot) {
  stone_reader_t r = {0, (uint32_t)slot * stone_slot_bytes};
  return gowin_upload_core_read(stone_read, &r, (uint32_t)stone_size, true);
}
#endif
