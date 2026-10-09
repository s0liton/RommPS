/* Save sync using RomM's device sync API (negotiate, transfer, complete). */
#ifndef ROMM_SYNC_H
#define ROMM_SYNC_H

#include <time.h>

#include "cJSON.h"

void sync_init(void);                    /* starts the worker thread */
void sync_request(const char *reason);   /* queued; repeats are merged */
/* After pairing: forgets the game and save ids of any server before. */
void sync_new_pairing(void);
int sync_is_running(void);
time_t sync_last_run(void);
cJSON *sync_status_json(void);           /* caller frees */

/* Resolves a conflict. keep is "local" or "server". */
int sync_resolve_conflict(const char *path, const char *keep, char *err, int en);

/* Moves a stray save to where the emulator reads it, if nothing is there yet. */
int sync_move_stray(const char *path, char *err, int en);

/* Records a play session; it is sent with the next sync. */
void sync_add_play_session(time_t start, time_t end, const char *title_id);

/* While a game runs, syncs only upload (except right after launch). */
void sync_set_game_running(int running);
int sync_game_running(void);

/* What a sync would do, without transferring anything. */
cJSON *sync_preview(char *err, int en);

/* Save versions and states RomM holds for a game. */
cJSON *sync_history(int rom_id, char *err, int en);
/* Restores a save version or a state ("save" or "state") into place. */
int sync_restore(const char *kind, int id, char *err, int en);

/* Save folders the last sync looked in, for the watcher. any_file marks
 * bundle folders, where every file counts. */
int sync_scanned_dirs(char dirs[][1024], int *any_file, int max);

/* Matches local games to RomM. */
void sync_scan_local_roms(void);

#endif
