/* BIOS files: what RomM keeps for a platform goes to every emulator that
 * plays it, the main one and the extras, as part of each sync.
 *
 * Where and under which names comes from the emulator's "firmware" rules (a
 * platform entry's, else its profile's, else the built-in catalog's; RetroArch
 * cores have built-in ones by core name):
 *
 *   "firmware": {
 *     "dir": "{bios}/dc",                  where they go; default bios_dir/firmware_subdir.
 *                                          {bios}, {root}, {rom_dir}, {save_dir}
 *     "names": {"<md5>": "dc_boot.bin"},   the name the emulator opens, by the file's md5;
 *                                          the first listed wins when several map to one name
 *     "only_named": true,                  nothing but the files in "names"
 *     "only_files": ["keys.txt"],          nothing but these names (as RomM has them)
 *     "by_ext": {".nca": "firmware"},      subfolders by extension; other extensions are left out
 *     "pick_one": ["<md5>", ...],          one file only, for emulators that take the first
 *                                          they find (PCSX2): the first of these RomM has, else
 *                                          a verified one; none if the folder already has one
 *     "from": {"arcade": ["naomi.zip"]},   files RomM keeps under another platform ("*": all)
 *     "skip": true                         the emulator needs none
 *   }
 *
 * A file already there that RommPS didn't put there is never replaced
 * (keys, an emulator's own eeprom). One it put there is replaced when RomM's
 * changes, unless it was changed here since: then it's the user's. Nothing
 * else is removed, but for "pick_one": the first time, of the files there
 * RomM has too (by md5), only the one in use stays. */
#ifndef ROMM_FIRMWARE_H
#define ROMM_FIRMWARE_H

#include "cJSON.h"
#include "profiles.h"

/* platforms: RomM's /api/platforms (NULL fetches it); only_platform: one
 * platform's id, or 0 for all. Returns the files added, -1 if RomM couldn't be
 * asked. *failed (may be NULL) counts the ones that didn't come down. */
int firmware_sync(const profile_map *maps, int n, const cJSON *platforms, int only_platform, int *failed);

/* Where the emulator keeps a BIOS file of this name: where RomM's copy goes
 * by the rules above, the user's own being there too (prod.keys in Eden's
 * keys folder). out gets the path even when there's nothing at it; 0 when
 * there is. */
int firmware_find(const profile_map *m, const char *name, char *out, size_t n);

#endif
