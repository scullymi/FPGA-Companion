/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_events.h
 *  @brief rcheevos over each good snapshot of the game RAM, see ra_events.c. */
#ifndef RA_EVENTS_H
#define RA_EVENTS_H

/** @brief Fills header bytes 0 to 5, the back channel for the diagnostic bars of the FPGA. */
void ra_events_header(unsigned char *hdr_tx);

/** @brief Makes rcheevos ready for a snapshot, a new rcheevos for another game. */
void ra_events_prepare(void);

/** @brief Evaluates one snapshot, ram is its game RAM of size bytes. */
void ra_events_frame(const unsigned char *ram, unsigned size);

#endif /* RA_EVENTS_H */
