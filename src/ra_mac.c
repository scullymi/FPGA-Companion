/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_mac.c
 *  @brief The device key, and HMAC-SHA256 tags over what the SD card keeps for hardcore.
 *
 *  The card can be taken out and edited on a computer. What decides hardcore
 *  unlocks there, the queue lines and the cached achievement set, therefore carries
 *  a tag that only this Pico can make: HMAC-SHA256 with a key of 32 random bytes,
 *  made at the first start and kept in a flash sector of its own. The key never
 *  goes to the card or the log.
 *
 *  It is no secret against someone who reads the Pico's flash with a debug probe or
 *  picotool, and nothing stops a modified firmware. That is the same bar as for any
 *  emulator built from its sources. It only keeps the card from deciding on its own.
 *
 *  The sector lies right below BTstack's two sectors, which keep the last one free
 *  (RP2350 erratum E10): 0x3FC000 on the 4 MB flash of the Pico 2 W. A firmware
 *  update does not touch it, a UF2 writes only its own blocks. */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "pico/unique_id.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/constant_time.h"
#include "ra_mac.h"

#define RA_MAC_OFFSET (PICO_FLASH_SIZE_BYTES - 4u * FLASH_SECTOR_SIZE)   /**< the key's sector, see above */
#define RA_MAC_MAGIC  "G20KKEY1"                                          /**< marks a record of this format */

/** The record at the start of the sector, one flash page. */
typedef struct {
  char    magic[8];     /**< RA_MAC_MAGIC */
  uint8_t key[32];      /**< the key */
  uint8_t check[32];    /**< SHA-256 over magic and key, tells a whole record from a torn one */
} record_t;

static uint8_t        key[32];
static ra_mac_state_t state = RA_MAC_NONE;

extern char __flash_binary_end;   // end of the firmware in flash, from the linker script

ra_mac_state_t ra_mac_state(void) { return state; }
bool ra_mac_ready(void) { return state == RA_MAC_CREATED || state == RA_MAC_PRESENT; }

static void record_check(const record_t *r, uint8_t *out) {
  mbedtls_sha256((const unsigned char *)r, sizeof(r->magic) + sizeof(r->key), out, 0);
}

ra_mac_state_t ra_mac_init(void) {
  const record_t *stored = (const record_t *)(XIP_BASE + RA_MAC_OFFSET);
  static uint8_t page[FLASH_PAGE_SIZE];   // static: main()'s stack is small before the scheduler
  record_t *r = (record_t *)page;
  uint8_t check[32];
  size_t i;

  // the sector must lie behind the firmware, a larger build would write into itself
  if((uintptr_t)&__flash_binary_end > XIP_BASE + RA_MAC_OFFSET) return state = RA_MAC_ERROR;

  // a record: take its key if it checks out, otherwise leave it alone
  if(!memcmp(stored->magic, RA_MAC_MAGIC, sizeof(stored->magic))) {
    record_check(stored, check);
    if(mbedtls_ct_memcmp(check, stored->check, sizeof(check)) != 0) return state = RA_MAC_ERROR;
    memcpy(key, stored->key, sizeof(key));
    return state = RA_MAC_PRESENT;
  }
  // anything but an erased sector is not ours to overwrite
  for(i = 0; i < sizeof(record_t); i++)
    if(((const uint8_t *)stored)[i] != 0xFF) return state = RA_MAC_ERROR;

  // a new key: two draws of pico_rand (fed by the RP2350's TRNG), the board id and
  // the time, all through SHA-256
  struct {
    rng_128_t              a, b;
    pico_unique_board_id_t id;
    uint64_t               us;
  } seed;
  get_rand_128(&seed.a);
  get_rand_128(&seed.b);
  pico_get_unique_board_id(&seed.id);
  seed.us = time_us_64();
  memset(page, 0xFF, sizeof(page));
  memcpy(r->magic, RA_MAC_MAGIC, sizeof(r->magic));
  mbedtls_sha256((const unsigned char *)&seed, sizeof(seed), r->key, 0);
  record_check(r, r->check);
  memset(&seed, 0, sizeof(seed));

  // erase and program with interrupts off. Nothing else runs yet: this is called
  // before the scheduler, the second core is not used by this firmware.
  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(RA_MAC_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(RA_MAC_OFFSET, page, FLASH_PAGE_SIZE);
  restore_interrupts(irq);

  // read back through XIP: only a record that arrived whole counts
  bool ok = !memcmp(stored, page, sizeof(record_t));
  if(ok) memcpy(key, r->key, sizeof(key));
  memset(page, 0, sizeof(page));
  return state = ok ? RA_MAC_CREATED : RA_MAC_ERROR;
}

/* HMAC-SHA256 over label, a newline and the data. */
static bool mac(const char *label, const void *data, size_t len, uint8_t *out) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  bool ok;
  if(!ra_mac_ready() || !md) return false;
  mbedtls_md_init(&ctx);
  ok = mbedtls_md_setup(&ctx, md, 1) == 0 &&
       mbedtls_md_hmac_starts(&ctx, key, sizeof(key)) == 0 &&
       mbedtls_md_hmac_update(&ctx, (const unsigned char *)label, strlen(label)) == 0 &&
       mbedtls_md_hmac_update(&ctx, (const unsigned char *)"\n", 1) == 0 &&
       mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, len) == 0 &&
       mbedtls_md_hmac_finish(&ctx, out) == 0;
  mbedtls_md_free(&ctx);
  return ok;
}

bool ra_mac_tag(const char *label, const void *data, size_t len, char *hex) {
  static const char digits[] = "0123456789abcdef";
  uint8_t t[32];
  unsigned i;
  if(!mac(label, data, len, t)) return false;
  for(i = 0; i < sizeof(t); i++) {
    hex[2 * i]     = digits[t[i] >> 4];
    hex[2 * i + 1] = digits[t[i] & 15];
  }
  hex[RA_MAC_HEX] = 0;
  return true;
}

bool ra_mac_check(const char *label, const void *data, size_t len, const char *hex) {
  char want[RA_MAC_HEX + 1];
  if(!hex || strlen(hex) != RA_MAC_HEX || !ra_mac_tag(label, data, len, want)) return false;
  return mbedtls_ct_memcmp(want, hex, RA_MAC_HEX) == 0;
}
