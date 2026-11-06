/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file games_file.h
 *  @brief The footer of a ROM file, see games_file.c. */
#ifndef GAMES_FILE_H
#define GAMES_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ff.h>

#define GAMES_FOOTER_SIZE    128   /**< bytes at the end of a ROM file, after the content */
#define GAMES_FOOTER_VERSION 1     /**< the layout below, byte 4 */
#define GAMES_SET_MAX        16    /**< set name in the footer, without the NUL */
#define GAMES_TITLE_MAX      48    /**< title in the footer, without the NUL */

/** @brief A valid footer, as games_footer_parse() found it. */
typedef struct {
  unsigned char board;                     /**< board id of the core that runs it, header byte 12 */
  unsigned char screen;                    /**< screen code of the manifest */
  uint32_t      content;                   /**< bytes before the footer, what goes to the core */
  char          set[GAMES_SET_MAX + 1];    /**< MAME set name */
  char          title[GAMES_TITLE_MAX + 1];/**< for the menu */
  unsigned char sha[32];                   /**< SHA-256 of the content */
} games_footer_t;

/** @brief CRC-32 as zlib computes it (IEEE, reflected, polynomial 0xEDB88320). */
uint32_t games_crc32(const void *data, size_t len);

/** @brief Checks the last GAMES_FOOTER_SIZE bytes of a file of file_size bytes.
 *
 *  Valid when the file holds at least a footer, magic "G20K", version, CRC-32 over
 *  bytes 0 to 123 and the content size equal to file_size minus the footer. out is
 *  filled only when valid. */
bool games_footer_parse(const unsigned char *buf, FSIZE_t file_size, games_footer_t *out);

/** @brief Reads and checks the footer of an open file. Caller holds sdc_lock.
 *
 *  One seek and one read of the last GAMES_FOOTER_SIZE bytes, then the file
 *  position is set back to where it was. False for a file without a valid footer
 *  or a read error. */
bool games_footer_read(FIL *f, games_footer_t *out);

/** @brief The page's order: by title without case, then by file name. Negative when a comes first. */
int games_order(const char *title_a, const char *name_a, const char *title_b, const char *name_b);

#endif /* GAMES_FILE_H */
