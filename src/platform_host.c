/* platform.h for macOS and Linux, used for development and tests.
 * ROMM_SYNC_DATA sets the data folder (default ./host-data). Writing a title id
 * to <data>/foreground pretends a game is running. */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "util.h"

static char g_data[PATH_MAX_LEN];

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

const char *plat_name(void) { return "host"; }

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
