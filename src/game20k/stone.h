/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file stone.h
 *  @brief The stepping stone: a jump straight to a core in the configuration flash, see stone.c. */
#ifndef STONE_H
#define STONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ram_mirror.h"

#define STONE_NONE (-1) /**< no slot or no way: the scan is unknown or no slot holds the board */

/** @brief The first slot that holds board, STONE_NONE when none does or known is false. */
int stone_slot(const unsigned char slots[RAM_MIRROR_SLOTS], bool known, unsigned char board);

/** @brief The hops of the ring from the slot of board from to the slot of board to, STONE_NONE
    when either has no slot. */
int stone_hops(const unsigned char slots[RAM_MIRROR_SLOTS], bool known, unsigned char from, unsigned char to);

/** @brief Writes address into the 0xD2 command at offset of the stepping stone, as far as it lies
    in buf, the bytes pos to pos + len - 1 of the bitstream. */
void stone_patch(unsigned char *buf, size_t len, size_t pos, size_t offset, uint32_t address);

/** The stepping stone with the jump address 0, from fpga/stone/stone.bin. The BL616 build only,
    g20k.py firmware generates the file, see game20k_bl616.cmake. */
extern const unsigned char stone_bits[];
extern const size_t stone_size;         /**< the bytes of stone_bits */
extern const size_t stone_d2;           /**< where its 0xD2 command starts */
extern const uint32_t stone_slot_bytes; /**< the size of a flash slot, from board.toml */

/** @brief Loads the stepping stone over JTAG with the address of slot and jumps there with a
    RECONFIG. False when the load failed. ENABLE_JTAG only. */
bool stone_jump(int slot);

#endif /* STONE_H */
