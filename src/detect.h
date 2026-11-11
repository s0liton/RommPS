/* Emulator detection for the setup wizard. */
#ifndef ROMM_DETECT_H
#define ROMM_DETECT_H

#include "cJSON.h"

/* Emulators found on the console, each with a ready-made profile. */
cJSON *detect_emulators(void);

/* Rebuilds profiles an older version of setup generated, so they pick up new
 * features. Profiles written by hand are left alone. Returns how many changed. */
int detect_refresh_profiles(cJSON *profiles);
/* The RetroArch core rule's keys (platform/retroarch_cores.json) for a
 * platform entry: all, or (missing_only) the missing ones and those the rule
 * alone decides. How many changed. */
int detect_apply_core_rules(cJSON *e, const char *system, const char *corename, int missing_only);
/* Whether a core's rule keeps its saves "apart": a twin of its own syncs them,
 * in a slot of its own, never the system's entry (MAME's NVRAM folders). */
int detect_core_apart(const char *system, const char *corename);

/* The built-in emulator catalog entry with this id (emulators.json), or NULL.
 * Parsed once; don't free. */
const cJSON *detect_catalog_entry(const char *id);

/* The libretro database a RetroArch platform entry plays ("Sony - PlayStation
 * 2"): its own "db", else the first that takes one of its RomM slugs. NULL if
 * none does. */
const char *detect_db_name(const cJSON *entry);
/* The system's ES-DE name ("psx", "gc", "dreamcast"): RetroArch 3.0+'s folder. */
const char *detect_esde_name(const cJSON *entry);

#endif
