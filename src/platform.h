/* Console-specific code: platform_ps5.c on the PS5, platform_host.c elsewhere. */
#ifndef ROMM_PLATFORM_H
#define ROMM_PLATFORM_H

#include <stddef.h>

/* Names the process, stops any older copy and raises privileges. */
int plat_init(void);

/* Show a toast notification on screen (host: prints to stderr). */
void plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Title id of the foreground app. 1 if one is running, 0 if not, -1 if unknown. */
int plat_running_title(char *title_id, size_t n);

/* Where config, state and logs are kept. */
const char *plat_data_dir(void);

/* CA bundle for libcurl, or NULL for the default. */
const char *plat_ca_bundle(void);

/* Install a home-screen tile that deep-links to http://127.0.0.1:<port>/.
 * Returns 0 on success, -1 if unsupported/failed. */
int plat_install_tile(int port);

/* Starts a payload through the ELF loader on 127.0.0.1:9021. The new copy
 * stops this one when it starts (plat_init). -1 with errno set on failure.
 * Host: writes <data>/launched.elf. */
int plat_launch_elf(const void *elf, size_t len);

/* "ps5" or "host". */
const char *plat_name(void);

/* The console's LAN address, or "". */
void plat_local_ip(char *buf, size_t n);

#endif
