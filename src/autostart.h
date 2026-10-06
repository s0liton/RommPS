/* Starting with the console through the loader (etaHEN on the PS5). Consoles
 * whose loader can't do that report supported: false. */
#ifndef ROMM_AUTOSTART_H
#define ROMM_AUTOSTART_H

#include <stddef.h>

#include "cJSON.h"

cJSON *autostart_status(void); /* {supported, loader, payload, loader_found, installed, enabled, path} */
/* Turns autostart on or off, installing the payload first if one is given. */
int autostart_set(int enable, const void *elf, size_t elf_len, char *err, int en);
/* Replaces an installed payload, keeping the old one as <payload>.bak.
 * 1 if replaced, 0 if none is installed, -1 on error. */
int autostart_replace(const void *elf, size_t elf_len, char *err, int en);

#endif
