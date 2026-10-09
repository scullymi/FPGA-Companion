/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file ra_text.h
 *  @brief Titles and descriptions of a set's achievements in one block, see ra_text.c. */
#ifndef RA_TEXT_H
#define RA_TEXT_H

#include <stdbool.h>
#include <stddef.h>

/** @brief The bytes a title and a description take in a block, each cut to what the menu shows. NULL counts as "". */
size_t ra_text_size(const char *title, const char *desc);

/** @brief Copies title and description, cut as ra_text_size() counts them, to *at and points *t and *d at the copies.
 *
 *  *at moves past them. False, and nothing copied, when *at is NULL or they do not fit before end. */
bool ra_text_put(char **at, const char *end, const char *title, const char *desc,
                 const char **t, const char **d);

/** @brief What ra_text_each() calls for one achievement, title and description decoded, NULL when missing. */
typedef void (*ra_text_fn)(void *arg, unsigned id, const char *title, const char *desc);

/** @brief Hands every achievement of a set text, the server's reply to r=patch, to fn.
 *
 *  rcheevos' own JSON reader decodes the strings as rc_api does for the parsed set, so a
 *  text takes the room ra_text_size() gave it there. False when the text holds no list of
 *  achievements or a string found no memory. */
bool ra_text_each(const char *json, size_t len, ra_text_fn fn, void *arg);

#endif
