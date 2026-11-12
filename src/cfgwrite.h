/* The catalog's "config_writes": settings an emulator needs to see what this
 * service brings it (its games folder in its game list, Eden's folder of
 * updates and DLC), written into the emulator's own config. Only while it's
 * closed: most write their whole config back when they exit. */
#ifndef ROMM_CFGWRITE_H
#define ROMM_CFGWRITE_H

/* Checks every few seconds, in the background, once the catalog has any
 * config_writes: at startup, and again whenever it changes (presets.c). */
void cfgwrite_init(void);

/* One pass now: how many config files changed. */
int cfgwrite_now(void);

#endif
