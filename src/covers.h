/* Covers and platform icons, kept on the console so each comes from RomM once.
 *
 * covers_fetch serves one (from the cache, or from RomM and then cached).
 * In the background, the games the library shows have their covers fetched
 * ahead of time (covers_warm), and with "cover_cache" on, every game's cover
 * is kept: a full pass after setup, then at most once a day only the covers
 * that are missing or changed (RomM's cover paths carry a change stamp). */
#ifndef ROMM_COVERS_H
#define ROMM_COVERS_H

#include <stddef.h>

#include "cJSON.h"

void covers_init(void);

/* path: a RomM asset path ("/assets/..."). 0 with the body (malloc'd, the
 * caller frees it) and its MIME type, 1 if RomM has no such asset, -1 on error. */
int covers_fetch(const char *path, char **body, size_t *len, const char **type);

/* Fetches a cover ahead of time, in the background, if it isn't cached. */
void covers_warm(const char *path);

/* Starts a full pass now (Settings, or after setup). */
void covers_refresh(void);

/* {enabled, state, done, total, cached_mb, limit_mb} */
cJSON *covers_status(void);

#endif
