/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ram_mirror.c
 *  @brief The RAM mirror on SPI target 5: header, snapshot and its checks, the back channel to the core. */
#include <string.h>

#include "../mcu_hw.h"
#include "../debug.h"
#include "../spi.h"     /* SPI_TARGET_RAM */
#include "banner.h"
#include "game_switch.h"
#include "ra_events.h"
#include "ra_patch.h"
#include "ra_task.h"
#include "ram_mirror.h"

/* RAM mirror on SPI target 5: each poll reads the header and, if there is a new
   snapshot, fetches the game RAM plus the oracle log, checks it, and sends the verdict
   as the first byte of the next transfer. The seven constants are the same for every
   game and must match ram_mirror_pkg.sv of the game20k FPGA platform. Layout 5: a
   header of 24 bytes, in it the core's reset count (byte 8), its diagnostic
   parameters (byte 9) and their complements (bytes 10 and 11), the board id of the
   core (byte 12, its complement in 13), the game RAM in the mirror in units of
   RAM_MIRROR_PAGE bytes (byte 14, its complement in 15) and the core's interface tag
   (bytes 16 and 17, low byte first, the complements in 18 and 19), which picks the
   menu (menus.h). Bytes 20 to 23 are 0. Each core sends its own length, the buffers
   here hold the largest one. */
#define RAM_MIRROR_LAYOUT    0x05                    /* header byte 4 */
#define RAM_MIRROR_HEAD      24
#define RAM_MIRROR_DATA_MAX  22528                   /* largest game RAM a core may announce */
#define RAM_MIRROR_LOG       1536                    /* oracle log, 512 x 3 bytes */
#define RAM_MIRROR_TAIL      8                       /* footer: frame, flags, checksum, log count */
#define RAM_MIRROR_BYTES_MAX (RAM_MIRROR_HEAD + RAM_MIRROR_DATA_MAX + RAM_MIRROR_LOG + RAM_MIRROR_TAIL)
#define RAM_MIRROR_PAGE      128                     /* unit of header byte 14, 176 pages at most */

static unsigned char ram_mirror_buf[RAM_MIRROR_BYTES_MAX];
static unsigned char ram_mirror_verdict = 0xA5;
static int ram_mirror_frame = -1;                    /* frame number of the last good snapshot */
static int ram_mirror_resets = -1;                   /* the core's reset count in it */
static unsigned char ram_mirror_seen[RAM_MIRROR_DATA_MAX / 8];
/* the running core's game RAM in the mirror, byte 14 x RAM_MIRROR_PAGE, 0 until a
   valid header was read; com_task only */
static unsigned      ram_mirror_data;
unsigned char ram_mirror_board;               /* its board id, byte 12, adopted with the length */
uint16_t      ram_mirror_tag;                 /* its interface tag, bytes 16 and 17, adopted with them */

/* Reads the header of one transfer, full duplex: hdr_tx goes to the FPGA meanwhile.
   Shared by the probe at boot and the poll. Returns 0 with SPI still open and the
   header in ram_mirror_buf, or the verdict with SPI ended: 0xE1 magic, 0xE2 layout
   (core and firmware of different releases, said once per layout seen; the banner
   still reaches an older core, the back channel in bytes 0 to 7 is the same), 0xE3
   bytes 12 to 19 (said once per refused set seen). Those carry the board id, the
   length and the interface tag with a complement each: a pair that does not match,
   a board of 0 or 255 (the stuck patterns 00 and FF fail here) or a length outside
   1..176 pages is refused and no body is read. The first valid set is adopted,
   together with the board id for the game's identity, and a later set that differs
   is refused too: the FPGA cannot change without a reconfiguration, after which
   ram_mirror_forget() reads the header anew. */
static unsigned char ram_mirror_header(const unsigned char *hdr_tx) {
  mcu_hw_spi_begin();
  mcu_hw_spi_tx_u08(SPI_TARGET_RAM);
  mcu_hw_spi_tx_u08(ram_mirror_verdict);     /* verdict on the previous transfer */
  mcu_hw_spi_txrx_block(hdr_tx, ram_mirror_buf, RAM_MIRROR_HEAD);

  if(ram_mirror_buf[0] != 'R' || ram_mirror_buf[1] != 'A' ||
     ram_mirror_buf[2] != 'C' || ram_mirror_buf[3] != 'H') {
    mcu_hw_spi_end(); return ram_mirror_verdict = 0xE1;
  }
  if(ram_mirror_buf[4] != RAM_MIRROR_LAYOUT) {
    static int told = -1;
    if(told != ram_mirror_buf[4]) {
      told = ram_mirror_buf[4];
      debugf("RAM mirror: the core sends layout %u, this firmware reads %u, "
             "flash core and firmware of the same release", ram_mirror_buf[4], RAM_MIRROR_LAYOUT);
      banner_show_kept(ram_mirror_buf[4] < RAM_MIRROR_LAYOUT ? "SYS: CORE TOO OLD" : "SYS: FIRMWARE TOO OLD",
                  false, false);
    }
    mcu_hw_spi_end(); return ram_mirror_verdict = 0xE2;
  }
  unsigned char board = ram_mirror_buf[12], pages = ram_mirror_buf[14];
  unsigned      data  = (unsigned)pages * RAM_MIRROR_PAGE;
  uint16_t      tag   = (uint16_t)(ram_mirror_buf[16] | ram_mirror_buf[17] << 8);
  bool valid = ram_mirror_buf[13] == (unsigned char)~board && ram_mirror_buf[15] == (unsigned char)~pages &&
               ram_mirror_buf[18] == (unsigned char)~ram_mirror_buf[16] &&
               ram_mirror_buf[19] == (unsigned char)~ram_mirror_buf[17] &&
               board != 0 && board != 255 && pages >= 1 && pages <= RAM_MIRROR_DATA_MAX / RAM_MIRROR_PAGE;
  if(valid && !ram_mirror_data) {
    ram_mirror_data  = data;
    ram_mirror_board = board;
    ram_mirror_tag   = tag;
    ra_patch_board(board);                 /* whichever of probe or poll sees it first feeds the identity */
    debugf("RAM mirror: layout %u, board %u, %u data bytes, interface tag %04x",
           RAM_MIRROR_LAYOUT, board, data, tag);
  } else if(!valid || data != ram_mirror_data || board != ram_mirror_board || tag != ram_mirror_tag) {
    /* once per distinct refused value, as the layout branch above does. A flag of
       its own for "nothing said yet": all 00 and all FF are values this branch
       refuses, so neither can serve as the sentinel */
    static bool     told_any;
    static uint32_t told[2];
    uint32_t cur[2] = { 0, 0 };
    for(int i = 0; i < 8; i++) cur[i / 4] = cur[i / 4] << 8 | ram_mirror_buf[12 + i];
    if(!told_any || told[0] != cur[0] || told[1] != cur[1]) {
      told_any = true;
      told[0] = cur[0];
      told[1] = cur[1];
      debugf("RAM mirror: header bytes 12..19 %08lx %08lx %s, snapshot refused",
             (unsigned long)cur[0], (unsigned long)cur[1],
             valid ? "differ from the adopted board, length and tag" : "invalid");
    }
    mcu_hw_spi_end(); return ram_mirror_verdict = 0xE3;
  }
  return 0;
}

/* Board id and interface tag before the menu and the game: the header only, up to 50
   tries 10 ms apart (half a second), at boot before the menu is read. ram_spi answers
   the header from constants while the core is still in reset. On failure (a core of
   another layout, no answer) the board stays 0, the poll adopts the first valid
   header it sees later, and the game's identity is decided as "board unknown". hdr_tx
   all zero: byte 5 is taken only when two transfers agree, and zero is its boot value.
   Returns 0 for a valid header, else the last verdict: 0xE1 no game20k core, 0xE2 a
   game20k core of another layout, 0xE3 one with a broken header.
   game20k: the probe also takes the core's reset count (byte 8, valid with its
   complement in byte 10) as the reference. The reset the boot ends comes later,
   so the poll sees it as the first game start, see ram_mirror_poll(). */
unsigned char ram_mirror_probe(void) {
  unsigned char hdr_tx[RAM_MIRROR_HEAD] = { 0 }, v = 0xE1;
  for(int i = 0; i < 50 && (!ram_mirror_data || ram_mirror_resets < 0); i++) {
    if(i) vTaskDelay(pdMS_TO_TICKS(10));
    v = ram_mirror_header(hdr_tx);
    if(v == 0) {
      if(ram_mirror_buf[10] == (unsigned char)~ram_mirror_buf[8]) ram_mirror_resets = ram_mirror_buf[8];
      mcu_hw_spi_end();                    /* the header is all the probe wants */
    }
  }
  if(!ram_mirror_data)
    debugf("RAM mirror: no valid layout %u header in 50 tries, the board is unknown", RAM_MIRROR_LAYOUT);
  return ram_mirror_data ? 0 : v;
}

/* game20k: another core came up, its header is read anew as at the start */
void ram_mirror_forget(void) {
  ram_mirror_data    = 0;
  ram_mirror_board   = 0;
  ram_mirror_tag     = 0;
  ram_mirror_resets  = -1;
  ram_mirror_frame   = -1;
  ram_mirror_verdict = 0xA5;
  ra_patch_board(0);
}

/* Read the header first. Byte 7 set means a harvest was running when the transfer
   started: the shadow is half old, half new, so skip it and retry on the next poll.
   That is normal, the verdict stays. From the verdict byte on, the FPGA starts no
   new harvest until chip select rises, so a clear byte 7 holds to the end. A harvest
   starting in exactly that clock tears the snapshot, the checksum (0xE6) catches it.
   Every offset in the body comes from the length the header announced. */
void ram_mirror_poll(void) {
  unsigned char hdr_tx[RAM_MIRROR_HEAD] = { 0 };
  banner_follow_game();
  ra_events_header(hdr_tx);

  // the banner, one character per poll in bytes 6 and 7
  banner_login();
  banner_lboard();
  if(switch_step()) return;   /* another core came up, the next poll reads it */
  banner_follow_game();       /* a failed core switch settles in switch_step() */
  banner_step(hdr_tx);

  /* Verdict: 0xA5 ok, otherwise the failed check. 0xE1 magic, 0xE2 layout, 0xE3
     board and length, 0xE4 frame number, 0xE6 checksum, 0xE7 log overflow, 0xE8
     and 0xE9 see below. */
  if(ram_mirror_header(hdr_tx)) return;      /* SPI ended, verdict set */
  if(ram_mirror_buf[7] != 0x00) {            /* harvest running, retry next poll */
    mcu_hw_spi_end(); return;
  }
  /* frame number counts harvests: same as the last good snapshot means nothing new */
  int frame = ram_mirror_buf[5] | (ram_mirror_buf[6] << 8);
  if(frame == ram_mirror_frame) {
    mcu_hw_spi_end(); return;
  }

  const unsigned d = ram_mirror_data;        /* this core's game RAM, checked by the header */
  mcu_hw_spi_rx_block(ram_mirror_buf + RAM_MIRROR_HEAD, d + RAM_MIRROR_LOG + RAM_MIRROR_TAIL);
  mcu_hw_spi_end();

  /* Same checksum the FPGA forms when it fills the FIFO, over game RAM and oracle
     log. Rotate before XOR so that the byte order counts. */
  unsigned short sum = 0;
  for(unsigned int i = RAM_MIRROR_HEAD; i < RAM_MIRROR_HEAD + d + RAM_MIRROR_LOG; i++)
    sum = (unsigned short)(((sum << 1) | (sum >> 15)) ^ ram_mirror_buf[i]);

  const unsigned char *ftr  = ram_mirror_buf + RAM_MIRROR_HEAD + d + RAM_MIRROR_LOG;
  unsigned short       want = (unsigned short)(ftr[4] | (ftr[5] << 8));

  /* Checksum before the underrun bit: the bit only says the FIFO ran empty, the
     checksum says whether data arrived wrong. */
  if(ram_mirror_buf[5] != ftr[0] ||
     ram_mirror_buf[6] != ftr[1])                 { ram_mirror_verdict = 0xE4; return; }
  if(sum != want)                                 { ram_mirror_verdict = 0xE6; return; }
  if(ftr[3] != 0x00)                              { ram_mirror_verdict = 0xE7; return; }

  /* Underrun with a good checksum did no harm: keep the snapshot, report 0xE8. */
  unsigned char note = (ftr[2] != 0x00) ? 0xE8 : 0xA5;

  /* Oracle. The log holds every game write during the harvest, and the snapshot must
     carry the last value written to each of those addresses. Walk the log backwards
     and check only the first hit per address. Other addresses cannot differ. */
  unsigned int n = (unsigned int)ftr[6] | (((unsigned int)ftr[7] & 3u) << 8);
  if(n > RAM_MIRROR_LOG / 3) n = RAM_MIRROR_LOG / 3;

  for(unsigned int i = 0; i < (d + 7) / 8; i++) ram_mirror_seen[i] = 0;   /* one bit per byte of this game's RAM */

  unsigned short bad = 0;
  for(int i = (int)n - 1; i >= 0; i--) {
    const unsigned char *e = ram_mirror_buf + RAM_MIRROR_HEAD + d + 3 * i;
    unsigned short flat = (unsigned short)(e[0] | (e[1] << 8));
    if(flat >= d) continue;                                /* cannot happen, guards the index */
    if(ram_mirror_seen[flat >> 3] & (1u << (flat & 7))) continue;
    ram_mirror_seen[flat >> 3] |= (unsigned char)(1u << (flat & 7));
    if(ram_mirror_buf[RAM_MIRROR_HEAD + flat] != e[2]) bad++;
  }
  if(bad) { ram_mirror_verdict = 0xE9; return; }     /* snapshot does not match the log */

  ra_events_prepare();
  /* The core counts the ends of its resets, S1 on the Nano as well as the menu's
     reset (byte 8, taken when the harvest ended). A new count is a new game: rcheevos
     starts over before it sees this snapshot, and only here, the Companion's own
     reset ('R') counts the same way. The probe took the reference, without one the
     first snapshot is the start. Byte 9, the core's diagnostic parameters, decides
     whether hardcore is possible at all, before the first frame is evaluated. */
  /* Bytes 10 and 11 carry the complements of 8 and 9: the checksum covers only the
     body, and a wrong bit here would reset rcheevos or block hardcore for nothing.
     A pair that does not match is left out for this snapshot. */
  if(ram_mirror_buf[10] == (unsigned char)~ram_mirror_buf[8] &&
     ram_mirror_buf[11] == (unsigned char)~ram_mirror_buf[9]) {
    int resets = ram_mirror_buf[8];
    if(resets != ram_mirror_resets) {
      ra_patch_core_reset();
      ram_mirror_resets = resets;
    }
    ra_task_core_flags(ram_mirror_buf[9]);
  }
  ra_events_frame(ram_mirror_buf + RAM_MIRROR_HEAD, ram_mirror_data);

  ram_mirror_frame   = frame;
  ram_mirror_verdict = note;
}
