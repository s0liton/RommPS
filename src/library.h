/* Library browsing and the download queue. */
#ifndef ROMM_LIBRARY_H
#define ROMM_LIBRARY_H

#include "cJSON.h"

void library_init(void);
void library_wake(void); /* re-check the queue, e.g. after the concurrency setting changed */

cJSON *library_platforms(char *err, int en);
/* Links the games a system already has into its extra emulators, in the background. */
void library_link_extras(void);

/* 1 for a file RomM lists as a game that isn't one (metadata.txt, artwork...). */
int library_not_game(const char *fs_name);
/* platform_id 0 searches every platform. */
cJSON *library_roms(int platform_id, const char *search, int offset, int limit, char *err, int en);
/* What RommPS shows about one game: release year, publishers, genres, players,
 * rating, summary, How Long to Beat, the user's last played, status and play time. */
cJSON *library_details(int rom_id, char *err, int en);
/* The games on this console, most recently played first. */
cJSON *library_installed(int limit);

/* name: the game's name as the client shows it ("" for "Game #<id>" until the job runs). */
int library_queue_rom(int rom_id, const char *name, char *err, int en);
int library_queue_firmware(int platform_id, char *err, int en);
int library_cancel(int id, char *err, int en);
int library_clear_finished(void);
int library_delete_rom(int rom_id, char *err, int en);
cJSON *library_downloads(void);
int library_busy(void); /* a download is queued or running */

#endif
