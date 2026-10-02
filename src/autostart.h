/* Starting with the console through etaHEN. */
#ifndef ROMM_AUTOSTART_H
#define ROMM_AUTOSTART_H

#include <stddef.h>

#include "cJSON.h"

cJSON *autostart_status(void); /* {etahen, installed, enabled, path} */
/* Turns autostart on or off, installing the payload first if one is given. */
int autostart_set(int enable, const void *elf, size_t elf_len, char *err, int en);

#endif
