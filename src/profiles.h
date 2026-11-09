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
 * "state_cores", "state_format", "state_slot0" and "state_core_setting" say
 * which save states an emulator can load (see profile_map); profiles saved
 * before they existed take them from the built-in catalog, by profile id.
 *
 * "save_layout" (psp, gci or wii) syncs a game's save folder or files as one zip;
 * "bundle_dirs" lists where to look, with {save_path}, {save_root} and {bios}.
 * "ps2" syncs each game's saves off PCSX2's memory cards, as one card per game
 * (ps2card.h), where "pcsx2_cards" says the emulator keeps them:
 *   {"dir": "{root}/memcards,{root}"           the card folder: the first that exists
 *    "game_ini": "{root}/settings/{rom_stem}.ini"  the game's own settings ({serial},
 *                                               {crc}: PCSX2's "SLUS-20552_248E6126.ini")
 *    "global_ini": "{root}/gs.ini"              the settings for every game, if any
 *    "format": "flat" | "ini"                   "MemoryCards/Slot1_Filename=X" lines, or
 *                                               an [MemoryCards] section
 *    "own_card": "{rom_stem}.ps2"               each game's own card, the one the game's
 *                                               settings get with one card per game
 *    "auto_card": "PS5/CardPerGame"             or: the emulator gives each game its own card
 *                                               while this setting isn't false (Crux2), named
 *    "game_db": "{root}/resources/GameIndex.yaml"}  as ps2card_db_card says
 * "memcards" can name the card folder as {card_dir}.
 * A platform entry can override any profile key. "sync_requires_option"
 * ({"key", "value", "default", "hint"}) turns save sync on only while a core
 * option has that value. Paths can use {root}, {platform}, {rom_dir} and {data},
 * and {user_dir}: the first folder of "user_dir" that exists (else its last),
 * for an emulator that keeps its files elsewhere when a folder is there
 * (Crux2: "/data/crux2,{root}"). With "user_dir_has", the one with that in it
 * comes first (PS5-ECAM's ".ecam-separated-v2", in each system folder it moved
 * to). A platform entry's own "user_dir" is that system's.
 *
 * A system is named by the first slug of its platform entry ("psx", "genesis";
 * the first slugs of detect.c's DB_MAP). When several emulators play one, the
 * user picks one and the others list it in "exclude": ["psx"], which drops
 * that platform entry from them, or in "extra": ["psx"], which keeps it as
 * an extra emulator for the system (see profile_map.extra).
 *
 * "mount_games": true says the emulator reads its games through its app
 * folder (/app0), "data" through the real /data out of its sandbox, where a
 * mount can show it the main
 * emulator's (see profile_map.mount_games).
 *
 * "update_dir" and "dlc_dir" are where a Switch game's update and DLC go,
 * downloaded with the game (library.c); the game's own files go straight
 * into rom_dir, never a folder of their own.
 *
 * "firmware" says how RomM's BIOS files for a platform reach the emulator
 * (firmware.h).
 *
 * "sync" is the user's choice for the emulator: "all" (saves and states, the
 * default), "saves" (no states) or "off" (games only). */
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
    /* Where a Switch game's update and DLC go ("update_dir", "dlc_dir"; the
     * DLC's defaults to the update's, and both to rom_dir). */
    char update_dir[PATH_MAX_LEN];
    char dlc_dir[PATH_MAX_LEN];
    char save_dir[PATH_MAX_LEN];
    char state_dir[PATH_MAX_LEN];
    char bios_dir[PATH_MAX_LEN];
    /* Where RomM's firmware goes in bios_dir: "" or a folder the core reads
     * ("firmware_subdir": "pcsx2/bios" for RetroArch's PS2 cores). */
    char firmware_subdir[32];
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
    /* The states an emulator resumes a game from on its own when it starts
     * (quick resume), as names in the state folder: set aside when the game's
     * save is replaced, so the save that came in is the one the game loads. */
    char resume_states[256];
    /* Save states from elsewhere come down only when the emulator can load
     * them: state_cores lists the cores whose states it takes (RomM
     * "emulator" names, default the platform's emulator). An emulator that
     * switches cores in its own settings ("state_core_setting": {"file",
     * "key", "values": {"0": "pcsx_rearmed", ...}}) takes only the current
     * one's. A value that lets the emulator choose ("auto": "0") is read
     * from the line its log names the core on ("log": file, "log_line":
     * "host: emulator ", "log_names": {"rearmed": "pcsx_rearmed"} by a part
     * of the name); "values" has the auto value's default for when the log
     * says nothing. state_format is "retroarch" (RetroArch's container, compressed
     * or not) or "raw" (the core's bare state, PSXS5); state_slot0 is the
     * extension of the first slot (".state", or ".state0"). */
    char state_cores[256];
    char state_format[16];
    char state_slot0[16];
    char core_file[PATH_MAX_LEN];
    char core_key[32];
    char core_values[256]; /* "0=pcsx_rearmed,2=mednafen_psx_hw" */
    char core_auto[16];
    char core_log[PATH_MAX_LEN];
    char core_log_line[64];
    char core_log_names[256]; /* "rearmed=pcsx_rearmed,beetle=mednafen_psx_hw" */
    /* Saves named differently by some of those cores ("save_name_by_core":
     * {"mednafen_psx_hw": "{rom_stem}_1{ext}"}): both names are the game's,
     * downloads take the current core's. "core=template;core=template". */
    char save_names_by_core[256];
    /* Several emulators for one system: the main one gets downloads first and
     * the extras ("extra": ["psx"] in their profile) a link to the same file.
     * Extras whose saves have the main one's format ("save_format": "psx-card")
     * share its save on RomM; the others keep their own slot (own_slot). */
    int extra;
    int own_slot;
    /* Another RetroArch core for a system its platform entry already plays
     * ("cores"): only its saves, in that core's folder and format. Never a
     * target for games, mounts or copies. */
    int twin;
    /* Mupen64Plus standalone's own N64 save files ("n64_split_dir"), and its
     * game database ("n64_ini"), for the names it gives them: the save the
     * sync sees is the .srm RommPS keeps for it in save_dir (n64.h). */
    char n64_split_dir[256];
    char n64_ini[256];
    /* Lists each game as a folder of its own ("game_folders": PS5X360): a
     * download of one file goes in <rom_dir>/<its name>/. */
    int game_folders;
    char save_format[32];
    /* An extra that reads its games through its own app folder ("mount_games":
     * true) or, out of its sandbox, through the real /data ("mount_games":
     * "data", by its catalog title id),
     * checked on the console for that emulator, plays the main one's through
     * a mount in its sandbox (plat_mounts_set) instead of copies:
     * rom_dir is then the main emulator's folder, own_rom_dir its own, and
     * mount_title and mount_into say where the mount goes. mount_note says
     * why one that could have a mount has copies instead. */
    int mount_games;
    int mount_blocked; /* games of its own in its folder, the only thing in the way (library.c moves RomM's to the main one) */
    char own_rom_dir[PATH_MAX_LEN];
    char mount_title[16];
    char mount_into[256];
    /* An extra keeping Dolphin's saves as the main emulator does (GameCube and
     * Wii folders): the main's folder is mounted on its own (saves_dir, inside
     * its sandbox saves_into, for app mount_title or saves_title), so both play
     * the same saves; bundle_dirs is then the main's, synced once. */
    char saves_from[512], saves_dir[512], saves_into[256], saves_title[16];
    char mount_note[200];
    /* Memory cards every game shares ("memcards": "{root}/Mcd001.ps2,..."),
     * backed up to RomM's memory cards (memcard.h). */
    char memcards[512];
    /* A front end's list of its games' ids ("game_ids": "{root}/data/game-ids.txt",
     * "<path>\t<id>" lines; PSFlyCast's), read before the disc: {game_id}. */
    char game_ids[PATH_MAX_LEN];
    /* Which disc header {game_id} is read from when no list has it
     * ("disc_id": "dreamcast": IP.BIN's product number). */
    char disc_id[16];
    /* Its memory cards ("cards"): the default and the modes it can keep
     * ("per_game", "shared"), where the user switches, how to tell which is
     * on, and which is (cards_mode, "" when the system has no cards). */
    char cards_default[12], cards_modes[24], cards_mode[12];
    char cards_change[200];
    char cards_option[64], cards_file[PATH_MAX_LEN], cards_key[64];
    char cards_per_game[96], cards_shared[64];
    /* The system's one card method (profiles_expand), and whether this
     * emulator keeps it: if not, none of its cards sync. */
    char cards_method[12];
    int cards_active;
    int sync_saves;
    int sync_states;   /* 0: the user turned states off for this emulator */
    int sync_off;      /* 1: the user turned save sync off for this emulator */
    /* RetroArch folder rules, [0] for saves and [1] for states:
     * in_content = next to the game, by_content = <dir>/<game folder>/,
     * by_core = <dir>/<core_name>/. Both sort flags nest content then core. */
    int in_content[2], by_content[2], by_core[2];
    char core_name[64]; /* core library name, e.g. "mGBA" */
    char save_layout[8];    /* "" for one file per game, or psp, gci, wii, ps2 (see bundle.h) */
    char bundle_dirs[512];  /* candidate folders for bundled saves, comma separated */
    /* switch: the Switch user whose saves sync ({switch_user} in bundle_dirs):
     * the profile's "switch_user", else the emulator's first user (a yuzu
     * fork's profiles.dat: the one its config has open). */
    char switch_user[40];
    /* x360: the player (XUID) whose saves sync ({xuid} in bundle_dirs): the
     * profile's "x360_user", else the emulator's player 1 ("x360_users", its
     * local-players.txt). */
    char xuid[20];
    /* ps2: "pcsx2_cards" (above). Folders are filled in, the game's names
     * ({rom_stem}, {serial}, {crc}) are left for each game. */
    char pc_dirs[512], pc_game_ini[PATH_MAX_LEN], pc_global_ini[PATH_MAX_LEN];
    char pc_format[8], pc_own_card[128], pc_auto_card[48], pc_game_db[PATH_MAX_LEN];
    /* The app's title id ("PPSA99203"), "" if unknown: nothing on its cards
     * changes while one of its processes runs (plat_app_alive). */
    char app_title[16];
    /* Why saves aren't synced for this platform, shown in the UI. */
    char off_note[200]; /* the preset's reason when its saves don't sync ("sync_note") */
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
/* ps2: the card folder, the first of pc_dirs that exists (else its last). */
void profile_card_dir(const profile_map *m, char *out, size_t n);

/* A folder setting from the emulator's retroarch.cfg ("playlist_directory"),
 * as a path; 0 if it has none (or isn't RetroArch). */
int profile_ra_dir(const profile_map *m, const char *key, char *out, size_t n);

/* Built-in default profiles (caller owns the returned array). */
cJSON *profiles_default(void);
/* Takes out built-in profiles this machine has no use for (Mednafen and PS2
 * ISOs on the Deck), from settings an older version saved. How many went. */
int profiles_drop_unsupported(cJSON *profiles);

/* The system a platform entry plays (its first RomM slug), and whether a
 * profile hands that system to another emulator ("exclude"). */
const char *profiles_system_key(const cJSON *entry);
/* The save format a "save_format" is a form of ("nds-dsv": DeSmuME's
 * footer on an "nds-raw" save): those share one save. */
const char *profiles_save_family(const char *format);
int profiles_excludes(const cJSON *profile, const char *key);

/* One entry per enabled profile and platform, with paths resolved.
 * Returns the count; profiles_free(*out). */
int profiles_expand(const cJSON *profiles, profile_map **out);
/* Frees what profiles_expand made. */
void profiles_free(profile_map *maps);
/* A system its emulator marked as moved ("user_dir_has"): the records of its
 * other folders follow to the one in use, and those folders lose the copies
 * of what that one has, and go once empty. */
void profiles_follow_moves(const cJSON *profiles);

/* The map's "firmware" rules (see firmware.h), from its platform entry or
 * profile, else the built-in catalog's; NULL if none. A custom profile with
 * no BIOS folder of its own gets {"skip": true}. Free it. */
cJSON *profile_firmware_rules(const cJSON *profiles, const profile_map *m);

/* Does the map handle a RomM platform with this slug / fs_slug? */
int profile_map_handles(const profile_map *m, const char *slug, const char *fs_slug);

/* Finds the first map (across profiles) that handles a platform. */
const profile_map *profile_find_for_platform(const profile_map *maps, int n, const char *slug,
                                             const char *fs_slug);

#endif
