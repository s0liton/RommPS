/* Save sync using RomM's device sync API (negotiate, transfer, complete). */
#ifndef ROMM_SYNC_H
#define ROMM_SYNC_H

#include <time.h>

#include "cJSON.h"
#include "profiles.h"

void sync_init(void);                    /* starts the worker thread */
void sync_request(const char *reason);   /* queued; repeats are merged */
/* After pairing: forgets the game and save ids of any server before. */
void sync_new_pairing(void);
int sync_is_running(void);
/* Waits for a sync running and holds the next back while files move. */
void sync_pause(void);
void sync_resume(void);
time_t sync_last_run(void);
/* 1, once, when a sync the server never answered is due to be tried again. */
int sync_retry_due(void);
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

/* Syncs one game's saves and states now, e.g. once it's downloaded, and
 * notifies what came down. Waits for a sync already running. -1 when
 * syncing isn't set up yet. */
typedef struct {
    int saves, states, conflicts, failed;
} sync_game_result;
/* problem: what's wrong with the game's file (game_file_problem), said in
 * the notification instead of how its saves went; NULL when nothing is. */
int sync_game(int rom_id, const char *name, const char *system, const char *problem, sync_game_result *out);

/* What's wrong with a game's file that keeps its saves from syncing, as the
 * end of a sentence ("the game file is incomplete (216 MB of 2.4 GB): ..."):
 * an Xbox image cut short. 1 then, and the file counts as said, so syncs
 * don't say it again; 0 when nothing's known to be wrong. */
int game_file_problem(const char *path, const char *layout, char *out, size_t n);
/* "Metal Gear Solid was downloaded for PlayStation", for the notification. */
void download_notice(char *out, size_t n, const char *name, const char *system);

/* What a sync would do, without transferring anything. */
cJSON *sync_preview(char *err, int en);

/* Save versions and states RomM holds for a game. */
cJSON *sync_history(int rom_id, char *err, int en);
/* Restores a save version or a state ("save" or "state") into place. */
int sync_restore(const char *kind, int id, char *err, int en);

/* The saves and states this console keeps in sync, by platform and game, each
 * with its status (synced, changed, conflict, missing); only_rom 0 for all. */
cJSON *sync_tracked(int only_rom);

/* Save folders the last sync looked in, for the watcher. any_file marks
 * bundle folders, where every file counts. */
int sync_scanned_dirs(char dirs[][1024], int *any_file, int max);

/* Matches local games to RomM. */
void sync_scan_local_roms(void);

/* A Switch file's title id as it stands ("0100F4300BF2C800" for an update,
 * switchid_kind_of says): its name's [title id], else the one inside it, read
 * with the emulator's prod.keys and kept in its state.json record. fresh:
 * read the file again, not the record. 0 when nothing says. */
uint64_t sync_nx_title(const profile_map *m, const char *path, int fresh);
/* A Switch file just recorded as downloaded: an update or DLC goes with its
 * game when that's here, and a game takes in the updates and DLC here that
 * were waiting for it. */
void sync_nx_link(const profile_map *m, const char *path);
/* Switch updates and DLC with nothing in their names to say so, left in the
 * games folder as games by older versions: what each is, its game, and its
 * move to update_dir or dlc_dir once the emulator is closed. Every sync does
 * it first; this is for when none runs (save sync off). */
void sync_nx_settle(const profile_map *maps, int n);

#endif
