/* Saves made of several files, synced as one zip per game:
 *   psp  PSP/SAVEDATA/<game id>*  folders (PPSSPP)
 *   gci  GC/<region>/Card A/ .gci files with the game id (Dolphin)
 *   wii  Wii/title/00010000/<id>/  folder (Dolphin) */
#ifndef ROMM_BUNDLE_H
#define ROMM_BUNDLE_H

#include <stdint.h>
#include <time.h>

#include "util.h"

typedef struct {
    char path[PATH_MAX_LEN];
    char name[512]; /* entry name in the zip */
} bundle_file;

typedef struct {
    char layout[8];
    char game_id[16];
    char dir[PATH_MAX_LEN]; /* where the files live and the zip is unpacked */
    bundle_file *files;
    int n, cap;
    time_t mtime;
    int64_t size;
} bundle;

int bundle_supported(const char *layout);

/* Picks the game's folder from candidate base folders (.../PSP/SAVEDATA,
 * .../User/GC or .../User/Wii), preferring one that exists, and lists its files. */
int bundle_open(bundle *b, const char *layout, const char *game_id, char bases[][PATH_MAX_LEN], int nbases);
void bundle_free(bundle *b);

/* RomM's hash for the zip this bundle would produce. */
int bundle_hash(const bundle *b, char out[33]);
int bundle_zip(const bundle *b, const char *zip_path);
/* Replaces the bundle's files with the zip's contents. */
int bundle_replace(bundle *b, const char *zip_path, const char *tmp_dir);

#endif
