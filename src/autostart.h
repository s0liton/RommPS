/* Starting with the console through the loader (etaHEN or onionHEN on the
 * PS5). Consoles whose loader can't do that report supported: false.
 *
 * It's always on. The payload goes into every installed loader's payload
 * folder, so it starts whichever loader the console runs, and copies other
 * tools keep (a payload manager's) are kept up to date. A copy is never
 * replaced by an older one.
 *
 * A console with no loader installed (jailbroken some other way, with a
 * payload manager doing the starting) has nowhere to set autostart up:
 * loader_found is false, enabling fails, and only those copies are kept. */
#ifndef ROMM_AUTOSTART_H
#define ROMM_AUTOSTART_H

#include <stddef.h>

#include "cJSON.h"

/* {supported, loader, payload, hint, loader_found, installed, enabled, path,
 *  version, copies: [{path, version, enabled}]}. installed, enabled, path and
 * version are about the running loader's folder. */
cJSON *autostart_status(void);
/* Turns autostart on in every installed loader's folder, installing the
 * payload there first if one is given. There's no off: RommPS needs it. */
int autostart_enable(const void *elf, size_t elf_len, char *err, int en);
/* Replaces every copy of the payload with this one (an update).
 * 1 if one was replaced, 0 if none is installed, -1 on error. */
int autostart_replace(const void *elf, size_t elf_len, char *err, int en);
/* At startup: brings every copy up to the newest one on the console, turns
 * autostart on in every installed loader that's missing it, and removes the
 * .bak files older versions left. */
void autostart_repair(void);

#endif
