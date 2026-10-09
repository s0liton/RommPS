/* Starting with the console through the loader (etaHEN or onionHEN on the
 * PS5). Consoles whose loader can't do that report supported: false.
 *
 * The payload goes into every installed loader's payload folder, so it starts
 * whichever loader the console runs, and copies other tools keep (a payload
 * manager's) are kept up to date. A copy is never replaced by an older one. */
#ifndef ROMM_AUTOSTART_H
#define ROMM_AUTOSTART_H

#include <stddef.h>

#include "cJSON.h"

/* {supported, loader, payload, hint, loader_found, installed, enabled, path,
 *  version, copies: [{path, version, enabled}]}. installed, enabled, path and
 * version are about the running loader's folder. */
cJSON *autostart_status(void);
/* Turns autostart on or off, installing the payload first if one is given.
 * Off removes the payload from every loader's folder. */
int autostart_set(int enable, const void *elf, size_t elf_len, char *err, int en);
/* Installs the payload wherever it belongs without turning autostart on or
 * off: in every loader's folder when it's on, else only over existing copies. */
int autostart_install(const void *elf, size_t elf_len, char *err, int en);
/* Replaces every copy of the payload with this one (an update).
 * 1 if one was replaced, 0 if none is installed, -1 on error. */
int autostart_replace(const void *elf, size_t elf_len, char *err, int en);
/* At startup: brings every copy up to the newest one on the console, sets up
 * any installed loader that's missing autostart while it's on, and removes
 * the .bak files older versions left. */
void autostart_repair(void);

#endif
