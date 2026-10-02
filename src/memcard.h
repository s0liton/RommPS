/* Backs up LRPS2's shared memory cards with RomM's memory card API (5.3.0+). */
#ifndef ROMM_MEMCARD_H
#define ROMM_MEMCARD_H

#include "cJSON.h"

int memcard_enabled(void);
/* Folders holding the cards, for the save watcher. Returns the count. */
int memcard_dirs(char dirs[][1024], int max);
/* Uploads cards that changed. Returns how many were uploaded. */
int memcard_backup(void);
/* Replaces a card with its latest backup on RomM. */
int memcard_restore(int slot, char *err, int en);
cJSON *memcard_status(void);

#endif
