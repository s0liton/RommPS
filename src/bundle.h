/* Saves made of several files, synced as one zip per game, in the layout of
 * RomM's own save units (rommapp/argosy-sigil), so other clients take them:
 *   psp   PSP/SAVEDATA/<game id>*  folders (PPSSPP); each folder by its name
 *   ps3   dev_hdd0/home/<user>/savedata/<title id>*  folders (RPCS3), the same way
 *   gci   GC/<region>/Card A/ .gci files with the game id (Dolphin)
 *   wii   Wii/title/00010000/<id>/  folder (Dolphin), as "<id>/..."
 *   3ds   title/<high>/<low>/data/00000001/ (Azahar), as
 *         "<high>/<low>/data/00000001/..." and its 00000001.metadata
 *   wiiu  usr/save/<high>/<low>/ (Cemu), as "<low>/meta/...",
 *         "<low>/user/common/..." and this console's account folder as
 *         "<low>/user/account/..."
 *   xbox  E:\UDATA\<title id>\ on xemu's hard disk image (xboxhdd.h), as
 *         "<title id>/..."; its base is the image, and its files are a copy
 *         kept in the data folder ("" for the base: that copy as it is)
 *   x360  <player>/<title id>/00000001/ and Headers/00000001/ (Xenia,
 *         PS5X360), as "<title id>/00000001/..." and
 *         "<title id>/Headers/00000001/..."
 *   switch  nand/user/save/0000000000000000/<user>/<title id>/ (the yuzu
 *         forks: Eden, Citron), as "<title id>/...", and the game's device
 *         save under the all-zero user as "device/<title id>/..."
 *         (argosy-sigil's yuzu unit). Its base is the user's folder.
 *   ryujinx  bis/user/save/<save id>/0/ (Ryujinx, Ryubing), the same unit:
 *         which folder is the user's save and which the device's, the save
 *         index says (bis/system/save/8000000000000000/0/imkvdb.arc). Its base
 *         is <Ryujinx's folder>/bis/user/save/<user id>; a game Ryujinx never
 *         ran has no save there yet (bundle.dir "").
 *   psvita  ux0/user/00/savedata/<save folder>/ (Vita3K), as "<save folder>/...":
 *         the one folder its param.sfo names (gameid_vita), argosy-sigil's
 *         vita3k unit
 *   ps4   savedata/<title id>/ (shadPS4), its save folders as "<title id>/<folder>/..."
 *   folder  <id>/ (MAME's nvram/<set>/), as "<id>/...": the game's one folder
 *   scummvm  <target>.<number> and <target>.s<number> files in ScummVM's
 *         save folder, as themselves at the zip's top ("monkey2.000")
 *   ps2   the game's save folders on the slots' PCSX2 folder cards, or a file
 *         card holding its saves; synced as sigil's one-game card (ps2card.h),
 *         not a zip. Its bases are the slot cards themselves.
 * Ids: the disc's "GALE01", a PS3 title "BLUS30443", a PS2 serial "SLUS-21059", a 3DS or Wii U title "0004000000066C00", or an Xbox or
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
    char dir2[PATH_MAX_LEN]; /* switch, ryujinx: the game's device save, "" when there's none */
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
/* The same, and in user the hash of that zip without the device save (switch,
 * ryujinx; out's again for the others): all RommPS 1.4.2 and older synced. */
int bundle_hashes(const bundle *b, char out[33], char user[33]);
/* switch, ryujinx: 1 when the game's device save is all there is, without the
 * user's (what a game leaves as it first starts, before anyone saves). */
int bundle_device_only(const bundle *b);
/* switch, ryujinx: whether a zip brings the user's save, not only the device
 * save, the way bundle_replace would place it. Unpacks into tmp_dir to look,
 * and clears it after. 1 or 0; -1 for a bad zip. */
int bundle_zip_has_user(const bundle *b, const char *zip_path, const char *tmp_dir);
int bundle_zip(const bundle *b, const char *zip_path);
/* Replaces the bundle's files with the zip's contents. -1 for a zip it
 * can't use, -2 for one holding none of the game's saves (psvita, ps4,
 * folder: other games' folders; scummvm: no file of the target's), with nothing replaced either way. */
int bundle_replace(bundle *b, const char *zip_path, const char *tmp_dir);
/* psp, ps3: the title ids of the save folders in a zip that aren't the
 * bundle's (its game id or also), comma-separated into out ("" for none).
 * Unpacks into tmp_dir to look, and clears it after. -1 for a bad zip. */
int bundle_other_ids(const bundle *b, const char *zip_path, const char *tmp_dir, char *out, size_t n);

#endif
