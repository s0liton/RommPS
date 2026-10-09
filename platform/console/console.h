/* Shared by platform/ps4 and platform/ps5. Both consoles run the same kind of
 * FreeBSD userland, so notifications, the process list, the data folder and
 * the ELF loader protocol work the same way; console.c implements those parts
 * of platform.h. Only plain libc here, so it builds on its own. */
#ifndef ROMM_CONSOLE_H
#define ROMM_CONSOLE_H

#include <stddef.h>
#include <sys/types.h>

#define CONSOLE_PROC_NAME "romm-sync.elf"
#define CONSOLE_DATA_DIR  "/data/romm-sync"
#define CONSOLE_CA_BUNDLE CONSOLE_DATA_DIR "/cacert.pem"
#define CONSOLE_ICON      CONSOLE_DATA_DIR "/icon0.png"

int console_mkdir_p(const char *path);

/* Visits every process. cb returns nonzero to stop; that value is returned,
 * or -1 if the list can't be read. */
typedef int (*console_proc_cb)(pid_t pid, const char *tdname, void *ctx);
int console_for_each_proc(console_proc_cb cb, void *ctx);

/* Names the main thread CONSOLE_PROC_NAME and kills any older copy. */
void console_single_instance(void);

/* Sends an ELF to a loader listening on 127.0.0.1:port. -1 with errno set. */
int console_send_elf(int port, const void *elf, size_t len);

/* PS4/PS5 title ids: 4 uppercase letters + 5 digits (CUSA00001, PPSA01234). */
int console_valid_title_id(const char *s);

/* plat_describe() with the title id of the process we run in ("" if none). */
void console_describe(char *buf, size_t n, const char *host_title);

#endif
