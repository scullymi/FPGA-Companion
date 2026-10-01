/*
 * ftpd — minimal FTP server
 */
#ifndef _FTPD_H
#define _FTPD_H

#include <stdbool.h>

void ftpd_init(void);

/* game20k: uploads (STOR) with a file open right now. The count changes only
 * under sdc_lock, read under it, it is exact. */
unsigned ftpd_uploads(void);

/* game20k: while held, a new upload is refused with 450. A restart of the Pico
 * holds them and waits for the ones that run: it would cut a file in the middle,
 * its directory entry and FAT half written. See restart_step() in main.c. */
void ftpd_hold_uploads(bool hold);

#endif
