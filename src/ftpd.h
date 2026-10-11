/*
 * ftpd — minimal FTP server
 */
#ifndef _FTPD_H
#define _FTPD_H

#include <stdbool.h>

void ftpd_init(void);

/* game20k: uploads (STOR) with a file open right now. The count changes only
 * under sdc_lock, read under it, it is exact. A restart of the Pico would cut
 * them, see switch_step() in main.c. */
unsigned ftpd_uploads(void);

#endif
