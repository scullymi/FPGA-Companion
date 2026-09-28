/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_mac.h
 *  @brief The device key and HMAC-SHA256 tags for what the SD card keeps, see ra_mac.c. */
#ifndef RA_MAC_H
#define RA_MAC_H

#include <stdbool.h>
#include <stddef.h>

#define RA_MAC_HEX 64   /**< a tag as lowercase hex, without its NUL */

/** @brief What ra_mac_init() found. */
typedef enum {
  RA_MAC_NONE,      /**< not initialised */
  RA_MAC_CREATED,   /**< no key was there, one was made and stored */
  RA_MAC_PRESENT,   /**< the stored key was read and checks out */
  RA_MAC_REPLACED,  /**< the sector held something else, e.g. another firmware's files: a new key was made */
  RA_MAC_ERROR      /**< the sector cannot be used (the firmware reaches into it, a partition table) or writing failed: no tags */
} ra_mac_state_t;

/** @brief Reads the device key from flash, or makes and stores one. Once, from main(), before the scheduler starts.
 *
 *  Making one erases and programs a flash sector with interrupts off, which is
 *  why it runs before anything else. A sector that holds no valid key, erased or
 *  not, gets a new one: without it there would be no hardcore, and a key that does
 *  not check out cannot check a tag either. */
ra_mac_state_t ra_mac_init(void);

/** @brief The result of ra_mac_init(). Any task. */
ra_mac_state_t ra_mac_state(void);

/** @brief Why ra_mac_init() gave RA_MAC_ERROR, as a short text for the log, else "". Any task. */
const char *ra_mac_error(void);

/** @brief True when tags can be made and checked. Any task. */
bool ra_mac_ready(void);

/** @brief The tag over label and data as lowercase hex into hex[RA_MAC_HEX + 1].
 *
 *  label keeps the kinds apart, e.g. "g20k-q1" for a queue line. False when there
 *  is no key. */
bool ra_mac_tag(const char *label, const void *data, size_t len, char *hex);

/** @brief True when hex is the tag over label and data. Compared in constant time. */
bool ra_mac_check(const char *label, const void *data, size_t len, const char *hex);

#endif /* RA_MAC_H */
