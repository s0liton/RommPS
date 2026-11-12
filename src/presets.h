#ifndef ROMM_PRESETS_H
#define ROMM_PRESETS_H

#include <stddef.h>

#include "cJSON.h"

/* 2: entries may have "vars" (settings read out of emulators' configs).
 * Format 1 files read the same; an older build takes no format 2 file, and
 * keeps its own catalog rather than one whose paths it can't fill in. */
#define PRESETS_FORMAT 2

/* Pick up the latest valid presets. Called at startup. */
void presets_init(void);

/* The catalog currently in use. Newer file takes its place and the old one stays valid in case it's still being used. */
const cJSON *presets_catalog(void);

/* Looks for newer presets in the background, and downloads, verifies, and applies them.*/
int presets_check(void);

/* Tells preset version, source (built-in) or (downloaded), info about the state, errors, and last checked time */
cJSON *presets_status(void);

/* Fills the catalog's machine placeholders ("{rdhome}") in place, with
 * plat_info's catalog_var, and each entry's "vars" (settings read out of the
 * emulator's own config: see eval_var in presets.c). Templates it doesn't
 * know ({root}) are left. */
void presets_resolve(cJSON *node);

/* tpl filled in for the catalog's entry entry_id: its own "vars" as they
 * are now, then the machine's placeholders ("{rdroms}/switch" is where
 * RetroDECK keeps Switch games on this machine). 1 only when all were known. */
int presets_fill(const char *entry_id, const char *tpl, char *out, size_t n);

/* 1 if a file or folder the catalog's vars looked at changed since (an
 * emulator installed, or its config saved): time for presets_refresh. */
int presets_vars_changed(void);

/* Fills the catalog's placeholders again, after plat_info's refresh says
 * the machine's folders changed. */
void presets_refresh(void);

/* Compare the version code within date to see if this contains newer presets. */
int presets_version_cmp(const char *a, const char *b);

#endif