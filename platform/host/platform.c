/* platform.h for macOS and Linux, used for development and tests.
 * ROMM_SYNC_DATA sets the data folder (default ./host-data). Writing a title id
 * to <data>/foreground pretends a game is running. It presents itself as a PS5,
 * so the end-to-end tests cover the PS5 paths and release assets. */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "platform.h"
#include "util.h"

static char g_data[PATH_MAX_LEN];

static const char *const HOMEBREW_DIRS[] = {NULL}; /* ROMM_SYNC_HOMEBREW */
static const char *const EMULATOR_DIRS[] = {NULL};

static const plat_info_t INFO = {
    .name = "host",
    .console = "PlayStation 5",
    .client = "romm-sync-ps5",
    .loader = "etaHEN",
    .payload = "romm-sync.elf",
    .manifest = "manifest.json",
    .manifest_sig = "manifest.sig",
    .autostart_dir = "./host-data/etaHEN/payloads", /* ROMM_SYNC_AUTOSTART in tests */
    .autostart_flag = "romm-sync.elf.auto_start",
    .homebrew_dirs = HOMEBREW_DIRS,
    .emulator_dirs = EMULATOR_DIRS,
    .retroarch_root = "/data/homebrew/PPSA99169",
    .mednafen_root = "/data/homebrew/Mednafen",
    .can_relaunch = 1,
};

const plat_info_t *plat_info(void) { return &INFO; }

int plat_init(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *d = getenv("ROMM_SYNC_DATA");
    str_copy(g_data, sizeof g_data, d && *d ? d : "./host-data");
    mkdir_p(g_data);
    return 0;
}

void plat_notify(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[NOTIFY] %s\n", msg);
}

int plat_running_title(char *title_id, size_t n) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, g_data, "foreground");
    char *txt = read_file(p, NULL);
    if (!txt) return 0;
    txt[strcspn(txt, "\r\n")] = 0;
    str_copy(title_id, n, txt);
    free(txt);
    return title_id[0] ? 1 : 0;
}

const char *plat_data_dir(void) { return g_data; }

const char *plat_ca_bundle(void) {
    const char *c = getenv("ROMM_SYNC_CA");
    return c && *c ? c : NULL;
}

int plat_install_tile(int port) {
    (void)port;
    return -1;
}

int plat_launch_elf(const void *elf, size_t len) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, g_data, "launched.elf");
    return write_file_atomic(p, elf, len);
}

const char *plat_name(void) { return INFO.name; }

void plat_describe(char *buf, size_t n) {
    struct utsname u;
    if (uname(&u)) snprintf(buf, n, "host build");
    else snprintf(buf, n, "host build on %s %s %s", u.sysname, u.release, u.machine);
}

void plat_local_ip(char *buf, size_t n) {
    buf[0] = 0;
    struct ifaddrs *ifs, *i;
    if (getifaddrs(&ifs) != 0) return;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
        if (ntohl(a->sin_addr.s_addr) >> 24 == 127) continue;
        inet_ntop(AF_INET, &a->sin_addr, buf, (socklen_t)n);
        break;
    }
    freeifaddrs(ifs);
}
