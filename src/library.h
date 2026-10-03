/* Library browsing and the download queue. */
#ifndef ROMM_LIBRARY_H
#define ROMM_LIBRARY_H

#include "cJSON.h"

void library_init(void);
void library_wake(void); /* re-check the queue, e.g. after the concurrency setting changed */

cJSON *library_platforms(char *err, int en);
cJSON *library_roms(int platform_id, const char *search, int offset, int limit, char *err, int en);

int library_queue_rom(int rom_id, char *err, int en);
int library_queue_firmware(int platform_id, char *err, int en);
int library_cancel(int id, char *err, int en);
int library_clear_finished(void);
int library_delete_rom(int rom_id, char *err, int en);
cJSON *library_downloads(void);
int library_busy(void); /* a download is queued or running */

#endif
