#ifndef SDC_H
#define SDC_H

#include <stdbool.h>
#include "config.h"
#include <ff.h>

// fatfs mounts the card under /sd
#define CARD_MOUNTPOINT "/sd"

typedef struct sdc_dir {
  char *name;
  unsigned long len;
  int is_dir;
  struct sdc_dir *next;
} sdc_dir_entry_t;

int sdc_init(void);
int sdc_wait_ready(unsigned char *status);   // game20k: the card of the core, see sdc.c
/** @brief sdc_image_open(): the core rejected the ROM file (wrong size for this board), it keeps the ROM it has. */
#define SDC_IMAGE_REJECTED -2
/** @brief sdc_image_open(): the file has no valid footer (games_file.h), an old ROM file, nothing is sent. */
#define SDC_IMAGE_OLD -3
int sdc_image_open(int drive, char *name);
sdc_dir_entry_t *sdc_readdir(int drive, char *name, const char *exts);
int sdc_handle_event(void);
void sdc_lock(void);
void sdc_unlock(void);
char *sdc_get_image_name(int drive);
char *sdc_get_cwd(int drive);
void sdc_set_cwd(int drive, char *path);
void sdc_set_default(int drive, const char *name);
void sdc_mount_defaults(void);
bool sdc_image_upload_in_progress(void);
bool sdc_check_for_pending_image_uploads(void);

#endif // SDC_H
