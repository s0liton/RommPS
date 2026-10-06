/* Console-specific code. Each build links one platform/<name>/platform.c:
 * ps5 (etaHEN), ps4 (GoldHEN), or host (macOS and Linux, for development). */
#ifndef ROMM_PLATFORM_H
#define ROMM_PLATFORM_H

#include <stddef.h>

/* What the shared code needs to know about the console it runs on. */
typedef struct {
    const char *name;     /* "ps5", "ps4" or "host" */
    const char *console;  /* sent to RomM as the device's platform: "PlayStation 5" */
    const char *client;   /* RomM "client" identifier: "romm-sync-ps5" */
    const char *loader;   /* what loads and autostarts payloads, for the UI: "etaHEN" */
    /* Release assets for this console: the payload, its manifest and signature. */
    const char *payload;
    const char *manifest;
    const char *manifest_sig;
    /* Autostart: the loader starts autostart_dir/payload at boot, when the file
     * autostart_flag (in the same folder) exists or is NULL. autostart_dir is
     * NULL if the loader can't. ROMM_SYNC_AUTOSTART overrides it, for tests. */
    const char *autostart_dir;
    const char *autostart_flag;
    const char *autostart_hint; /* what the user does in the loader's menu, or NULL */
    const char *loader_dir;     /* present when the loader is installed; NULL: autostart_dir's parent */
    /* Folders to look in for emulators. homebrew_dirs hold one app per subfolder;
     * emulator_dirs are emulator folders themselves. Both end with NULL. */
    const char *const *homebrew_dirs;
    const char *const *emulator_dirs;
    /* Where the built-in emulator profiles point before setup finds the real folders. */
    const char *retroarch_root;
    const char *mednafen_root;
    /* Standalone emulators this console has, as a JSON array (detect.c), or NULL. */
    const char *emulator_catalog;
    int has_tile; /* plat_install_tile can add a home-screen tile */
    int detach;   /* main() must return to the loader: the app runs on a thread */
    int can_relaunch; /* plat_launch_elf works, so an update can restart in place */
} plat_info_t;

const plat_info_t *plat_info(void);

/* Names the process, stops any older copy and raises privileges. 1 if another
 * copy is running and this one should quit, -1 on a problem, 0 otherwise. */
int plat_init(void);

/* Show a toast notification on screen (host: prints to stderr). */
void plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Title id of the foreground app. 1 if one is running, 0 if not, -1 if unknown. */
int plat_running_title(char *title_id, size_t n);

/* Where config, state and logs are kept. */
const char *plat_data_dir(void);

/* CA bundle for libcurl, or NULL for the default. */
const char *plat_ca_bundle(void);

/* Install a home-screen tile that opens http://127.0.0.1:<port>/.
 * Returns 0 on success, -1 if unsupported/failed. */
int plat_install_tile(int port);

/* Starts a payload through the console's loader on loopback, if can_relaunch.
 * The new copy
 * stops this one when it starts (plat_init). -1 with errno set on failure.
 * Host: writes <data>/launched.elf. */
int plat_launch_elf(const void *elf, size_t len);

/* plat_info()->name. */
const char *plat_name(void);

/* The console's LAN address, or "". */
void plat_local_ip(char *buf, size_t n);

/* One line for logs and diagnostics: firmware, and the process the payload
 * runs in. */
void plat_describe(char *buf, size_t n);

#endif
