/* Saves made of several files, synced as one zip per game, in the layout of
 * RomM's own save units (rommapp/argosy-sigil), so other clients take them:
 *   psp   PSP/SAVEDATA/<game id>*  folders (PPSSPP); each folder by its name
 *   gci   GC/<region>/Card A/ .gci files with the game id (Dolphin)
 *   wii   Wii/title/00010000/<id>/  folder (Dolphin), as "<id>/..."
 *   3ds   title/<high>/<low>/data/00000001/ (Azahar), as
 *         "<high>/<low>/data/00000001/..." and its 00000001.metadata
 *   wiiu  usr/save/<high>/<low>/ (Cemu), as "<low>/meta/...",
 *         "<low>/user/common/..." and this console's account folder as
 *         "<low>/user/account/..."
 *   xbox  E:\UDATA\<title id>\ on xemu's hard disk image (xboxhdd.h), as
 *         "<title id>/..."; its base is the image, and its files are a copy
 *         kept in the data folder ("" for the base: that copy as it is). Or
 *         a hard disk kept as folders (XPSemu 3's hdd, Partition1 being E:):
 *         that folder is the base, the files are its own, the zip the same
 *   x360  <player>/<title id>/00000001/ and Headers/00000001/ (Xenia,
 *         PS5X360), as "<title id>/00000001/..." and
 *         "<title id>/Headers/00000001/..."
 *   ps2   the game's save folders on the slots' PCSX2 folder cards, or a file
 *         card holding its saves; synced as sigil's one-game card (ps2card.h),
 *         not a zip. Its bases are the slot cards themselves.
 * Ids: the disc's "GALE01", a PS2 serial "SLUS-21059", a 3DS or Wii U title "0004000000066C00", or an Xbox or
 * Xbox 360 title "4D530004". A
 * zip from elsewhere is taken in other forms too (no root folder, Cemu's own
 * export, the whole path from the SD card). */
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
    char game_id[24];
    char base[PATH_MAX_LEN]; /* the candidate folder picked */
    char dir[PATH_MAX_LEN];  /* where the files live and the zip is unpacked */
    char account[16];        /* wiiu: this console's account folder, "80000001" */
    /* psp, ps3: more title ids its save folders start with, comma-separated.
     * Re-releases love saving under the original's id: Midnight Club: LA's
     * Complete Edition (BLUS30442) writes BLUS30190MC4_SAV. */
    char also[64];
    bundle_file *files;
    int n, cap;
    time_t mtime;
    int64_t size;
} bundle;

int bundle_supported(const char *layout);

/* Picks the game's folder from candidate base folders (.../PSP/SAVEDATA,
 * .../User/GC, .../User/Wii, the 3DS SD card's .../title or Cemu's
 * mlc01/usr/save), preferring one that exists, and lists its files. also:
 * see bundle.also, NULL for none. */
int bundle_open(bundle *b, const char *layout, const char *game_id, const char *also, char bases[][PATH_MAX_LEN],
                int nbases);
void bundle_free(bundle *b);

/* RomM's hash for the zip this bundle would produce. */
int bundle_hash(const bundle *b, char out[33]);
int bundle_zip(const bundle *b, const char *zip_path);
/* Replaces the bundle's files with the zip's contents. -1 for a zip it
 * can't use, with nothing replaced. */
int bundle_replace(bundle *b, const char *zip_path, const char *tmp_dir);
/* psp, ps3: the title ids of the save folders in a zip that aren't the
 * bundle's (its game id or also), comma-separated into out ("" for none).
 * Unpacks into tmp_dir to look, and clears it after. -1 for a bad zip. */
int bundle_other_ids(const bundle *b, const char *zip_path, const char *tmp_dir, char *out, size_t n);

#endif
