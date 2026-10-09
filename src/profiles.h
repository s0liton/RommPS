/* Emulator profiles: where an emulator keeps games, saves, states and BIOS
 * files, and which RomM platforms it plays. Stored as JSON in config.json.
 *
 * {
 *   "id": "retroarch", "name": "RetroArch", "enabled": true,
 *   "root": "/data/homebrew/PPSA99169",
 *   "retroarch_cfg": "{root}/config/retroarch.cfg,{root}/retroarch.cfg",
 *   "rom_dir": "{root}/content/{platform}",
 *   "save_dir": "{root}/savefiles",
 *   "state_dir": "{root}/savestates",
 *   "bios_dir": "{root}/system",
 *   "save_exts": ".srm,.sav,.rtc",
 *   "state_exts": ".state*",
 *   "save_name": "{rom_stem}{ext}",
 *   "platforms": [
 *     { "romm": ["snes", "sfam"], "dir": "snes", "emulator": "snes9x", "core_name": "Snes9x" }
 *   ]
 * }
 *
 * "save_stems" and "state_stems" (comma separated templates) give a game's
 * files more names to match, and "state_name" names downloaded states:
 * {serial} (a PS1 disc's, "SCUS-94194"), {rom_fnv64} (FNV-1a 64 of the game
 * file, in hex) and {rom_stem_safe} (the file name, non-alphanumerics as "_").
 *
 * "save_layout" (psp, gci or wii) syncs a game's save folder or files as one zip;
 * "bundle_dirs" lists where to look, with {save_path}, {save_root} and {bios}.
 * A platform entry can override any profile key. "sync_requires_option"
 * ({"key", "value", "default", "hint"}) turns save sync on only while a core
 * option has that value. Paths can use {root}, {platform}, {rom_dir} and {data}.
 *
 * A system is named by the first slug of its platform entry ("psx", "genesis";
 * the first slugs of detect.c's DB_MAP). When several emulators play one, the
 * user picks one and the others list it in "exclude": ["psx"], which drops
 * that platform entry from them, or in "extra": ["psx"], which keeps it as
 * an extra emulator for the system (see profile_map.extra). */
#ifndef ROMM_PROFILES_H
#define ROMM_PROFILES_H

#include "cJSON.h"
#include "util.h"

typedef struct {
    char profile_id[32];
    char profile_name[64];
    char emulator[64];      /* value sent as RomM "emulator" */
    char platform_dir[64];  /* {platform} value */
    char romm_slugs[256];   /* comma separated RomM slugs / fs_slugs */
    char rom_dir[PATH_MAX_LEN];
    char save_dir[PATH_MAX_LEN];
    char state_dir[PATH_MAX_LEN];
    char bios_dir[PATH_MAX_LEN];
    char save_exts[128];
    char state_exts[128];
    char save_name[128];
    /* More names a game's files can have, as templates (comma separated):
     * "{serial}_1" for cards named by disc serial, "{rom_fnv64}" for saves
     * named by the game file's FNV-1a hash, "{rom_stem_safe}" for the name
     * with every non-alphanumeric character as "_". state_name names a state
     * downloaded for a game that has none here. */
    char save_stems[256];
    char state_stems[256];
    char state_name[128];
    /* Several emulators for one system: the main one gets downloads first and
     * the extras ("extra": ["psx"] in their profile) a link to the same file.
     * Extras whose saves have the main one's format ("save_format": "psx-card")
     * share its save on RomM; the others keep their own slot (own_slot). */
    int extra;
    int own_slot;
    char save_format[32];
    /* Memory cards every game shares ("memcards": "{root}/Mcd001.ps2,..."),
     * backed up to RomM's memory cards (memcard.h). */
    char memcards[512];
    int sync_saves;
    /* RetroArch folder rules, [0] for saves and [1] for states:
     * in_content = next to the game, by_content = <dir>/<game folder>/,
     * by_core = <dir>/<core_name>/. Both sort flags nest content then core. */
    int in_content[2], by_content[2], by_core[2];
    char core_name[64]; /* core library name, e.g. "mGBA" */
    char save_layout[8];    /* "" for one file per game, or psp, gci, wii (see bundle.h) */
    char bundle_dirs[512];  /* candidate folders for bundled saves, comma separated */
    /* Why saves aren't synced for this platform, shown in the UI. */
    char sync_note[200];

    /* Kept for per-game overrides (see profile_rules_for). */
    char root[PATH_MAX_LEN];
    char ra_cfg_dir[PATH_MAX_LEN];
    int explicit_dir[2];
    int base_sync_saves;
    char req_key[64], req_value[64], req_default[64], req_hint[200];
} profile_map;

/* Save rules for one game after RetroArch's per-core, per-folder and per-game
 * overrides (config/<core>/<name>.cfg and .opt). */
typedef struct {
    char save_dir[PATH_MAX_LEN];
    char state_dir[PATH_MAX_LEN];
    int in_content[2], by_content[2], by_core[2];
    int sync_saves;
    char sync_note[200];
} rom_rules;

void profile_rules_for(const profile_map *m, const char *rom_path, rom_rules *out);

/* Built-in default profiles (caller owns the returned array). */
cJSON *profiles_default(void);

/* The system a platform entry plays (its first RomM slug), and whether a
 * profile hands that system to another emulator ("exclude"). */
const char *profiles_system_key(const cJSON *entry);
int profiles_excludes(const cJSON *profile, const char *key);

/* One entry per enabled profile and platform, with paths resolved.
 * Returns the count; free *out. */
int profiles_expand(const cJSON *profiles, profile_map **out);

/* Does the map handle a RomM platform with this slug / fs_slug? */
int profile_map_handles(const profile_map *m, const char *slug, const char *fs_slug);

/* Finds the first map (across profiles) that handles a platform. */
const profile_map *profile_find_for_platform(const profile_map *maps, int n, const char *slug,
                                             const char *fs_slug);

#endif
