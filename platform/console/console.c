/* platform.h parts shared by the PS4 and PS5 builds (console.h).
 *
 * Adapted from John Törnblom's GPLv3 ftpsrv and websrv: process lookup and the
 * single-instance check (ftpsrv main-orbis.c and main-prospero.c, which use the
 * same kinfo_proc offsets), notifications and the LAN address (websrv). */
#include "console.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>

#include "platform.h"

/* libkernel: same layout as ftpsrv/websrv notify.c, on both consoles */
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);


int console_mkdir_p(const char *path) {
    char tmp[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

/* sysctl(KERN_PROC, KERN_PROC_PROC), read with the offsets ftpsrv uses for the
 * console kinfo_proc: ki_structsize @0, ki_pid @72, ki_tdname @447
 * (TDNAMLEN+1 = 17 bytes). */
int console_for_each_proc(console_proc_cb cb, void *ctx) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t buf_size = 0;
    uint8_t *buf;
    int ret = 0;

    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) || buf_size == 0) return -1;
    buf_size += buf_size / 4; /* processes may appear between the two calls */
    if (!(buf = malloc(buf_size))) return -1;
    if (sysctl(mib, 4, buf, &buf_size, NULL, 0)) {
        free(buf);
        return -1;
    }
    for (uint8_t *ptr = buf; ptr + 464 <= buf + buf_size;) {
        int ki_structsize = *(int *)ptr;
        pid_t ki_pid = *(pid_t *)&ptr[72];
        char tdname[17];
        if (ki_structsize < 464 || ptr + ki_structsize > buf + buf_size) break;
        memcpy(tdname, &ptr[447], 16);
        tdname[16] = 0;
        ptr += ki_structsize;
        if ((ret = cb(ki_pid, tdname, ctx))) break;
    }
    free(buf);
    return ret;
}

struct find_ctx { const char *name; pid_t self; pid_t found; };

static int find_cb(pid_t pid, const char *tdname, void *vctx) {
    struct find_ctx *c = vctx;
    if (pid != c->self && !strcmp(tdname, c->name)) {
        c->found = pid;
        return 1;
    }
    return 0;
}

static pid_t find_pid(const char *name) {
    struct find_ctx c = {name, getpid(), -1};
    if (console_for_each_proc(find_cb, &c) < 0) return -1;
    return c.found;
}

void console_single_instance(void) {
    pid_t pid;
    int tries = 0;

    /* Name the main thread so find_pid() of the next instance sees us. */
    syscall(SYS_thr_set_name, -1, CONSOLE_PROC_NAME);

    while ((pid = find_pid(CONSOLE_PROC_NAME)) > 0) {
        if (kill(pid, SIGKILL) && errno != ESRCH) break;
        sleep(1);
        if (++tries >= 10) break;
    }
}

int console_send_elf(int port, const void *elf, size_t len) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    const char *p = elf;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            int e = w < 0 ? errno : EPIPE;
            close(fd);
            errno = e;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    shutdown(fd, SHUT_WR);
    close(fd);
    return 0;
}

int console_valid_title_id(const char *s) {
    for (int i = 0; i < 9; i++) {
        char ch = s[i];
        if (i < 4 ? !(ch >= 'A' && ch <= 'Z') : !(ch >= '0' && ch <= '9')) return 0;
    }
    return 1;
}

/* kern.sdk_version is 0xMMmmxxxx with the firmware version in BCD: 0x09008031 is 9.00. */
void console_describe(char *buf, size_t n, const char *host_title) {
    uint32_t sdk = 0;
    size_t len = sizeof sdk;
    char fw[32] = "unknown";
    if (!sysctlbyname("kern.sdk_version", &sdk, &len, NULL, 0) && sdk)
        snprintf(fw, sizeof fw, "%x.%02x (0x%08x)", sdk >> 24, (sdk >> 16) & 0xff, sdk);
    snprintf(buf, n, "%s firmware %s, pid %d, uid %d, host process %s", plat_info()->console, fw,
             (int)getpid(), (int)getuid(), host_title && host_title[0] ? host_title : "none");
}

void plat_notify(const char *fmt, ...) {
    notify_request_t req;
    va_list ap;

    memset(&req, 0, sizeof req);
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

const char *plat_data_dir(void) {
    console_mkdir_p(CONSOLE_DATA_DIR);
    return CONSOLE_DATA_DIR;
}

const char *plat_ca_bundle(void) {
    struct stat st;
    if (!stat(CONSOLE_CA_BUNDLE, &st) && st.st_size > 0) return CONSOLE_CA_BUNDLE;
    return NULL;
}

const char *plat_name(void) { return plat_info()->name; }

void plat_local_ip(char *buf, size_t n) {
    struct ifaddrs *ifaddr = NULL;
    char ip[INET_ADDRSTRLEN];

    if (!buf || !n) return;
    buf[0] = 0;
    if (getifaddrs(&ifaddr) == -1) return;

    /* websrv sys.c: first AF_INET that is not loopback and not 0.x */
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_name && !strncmp("lo", ifa->ifa_name, 2)) continue;
        struct sockaddr_in *in = (struct sockaddr_in *)ifa->ifa_addr;
        if (!inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip)) continue;
        if (!strncmp("0.", ip, 2)) continue;
        snprintf(buf, n, "%s", ip);
        break;
    }
    freeifaddrs(ifaddr);
}
