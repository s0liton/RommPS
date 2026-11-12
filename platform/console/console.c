/* platform.h parts shared by the PS4 and PS5 builds (console.h).
 *
 * Adapted from John Törnblom's GPLv3 ftpsrv and websrv: process lookup and the
 * single-instance check (ftpsrv main-orbis.c and main-prospero.c, which use the
 * same kinfo_proc offsets), notifications and the LAN address (websrv). */
#include "console.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/file.h>
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

/* Kills every other process called CONSOLE_PROC_NAME, giving up after 10 s. */
static void kill_by_name(void) {
    pid_t pid;
    int tries = 0;
    while ((pid = find_pid(CONSOLE_PROC_NAME)) > 0) {
        if (kill(pid, SIGKILL) && errno != ESRCH) break;
        sleep(1);
        if (++tries >= 10) break;
    }
}

struct pid_ctx { pid_t pid; };

static int has_pid_cb(pid_t pid, const char *tdname, void *vctx) {
    struct pid_ctx *c = vctx;
    return pid == c->pid && !strcmp(tdname, CONSOLE_PROC_NAME);
}

/* Whether this copy should take over from one running `have`. A version that
 * doesn't parse (a hand-made build) is sent on purpose, so it takes over. A
 * release takes over from its dev builds (src/version.h). */
static int takes_over(const char *mine, const char *have) {
    if (!version_valid(mine) || !version_valid(have)) return 1;
    return version_cmp(mine, have) > 0;
}

/* Held for as long as this copy runs; the kernel lets go when the process
 * ends, however it ends. The file says which version holds it and its pid. */
static int g_lock_fd = -1;

/* One line in the log, written before the log is set up: a copy that quits
 * here would otherwise leave no trace. */
static void note(const char *fmt, ...) {
    char line[256], when[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    int n = snprintf(line, sizeof line, "%s [INFO] ", when);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    int fd = open(CONSOLE_DATA_DIR "/romm-sync.log", O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd < 0) return;
    write(fd, line, (size_t)n);
    close(fd);
}

static int lock_wait(int fd, int seconds) {
    for (int i = 0; i <= seconds * 4; i++) {
        if (!flock(fd, LOCK_EX | LOCK_NB)) return 0;
        usleep(250 * 1000);
    }
    return -1;
}

int console_single_instance(const char *version) {
    console_mkdir_p(CONSOLE_DATA_DIR);
    int fd = open(CONSOLE_LOCK_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        /* No lock to be had: run, as versions before the lock did. */
        kill_by_name();
        syscall(SYS_thr_set_name, -1, CONSOLE_PROC_NAME);
        return 0;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        char have[32] = "";
        pid_t holder = 0;
        int alive = 0;
        /* A copy that took the lock a moment ago may not have said who it is
         * yet; until it does, the file still names the copy before it. */
        for (int tries = 0; tries < 8 && !alive; tries++) {
            char buf[64] = "";
            ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
            if (n > 0) buf[n] = 0;
            have[0] = 0;
            holder = 0;
            sscanf(buf, "%31s %d", have, &holder);
            struct pid_ctx c = {holder};
            alive = holder > 0 && console_for_each_proc(has_pid_cb, &c) == 1;
            if (!alive) usleep(250 * 1000);
        }
        /* Held, but by no copy of ours: RommPS checking whether one runs.
         * It lets go at once. */
        if (!alive && !lock_wait(fd, 3)) goto locked;
        if (!takes_over(version, have)) {
            note("romm-sync %s not started: %s is already running (pid %d)", version, have, (int)holder);
            close(fd);
            return 1;
        }
        /* Older: stop it, but only if that pid is still RomM Sync. */
        note("romm-sync %s replacing %s (pid %d)", version, have, (int)holder);
        struct pid_ctx c = {holder};
        if (holder > 0 && console_for_each_proc(has_pid_cb, &c) == 1) kill(holder, SIGKILL);
        if (lock_wait(fd, 10)) {
            note("romm-sync %s not started: %s didn't stop", version, have);
            close(fd);
            return 1;
        }
    }
locked:;
    /* Say who holds the lock first, for any copy starting right now. */
    char line[64];
    int len = snprintf(line, sizeof line, "%s %d\n", version, (int)getpid());
    if (!ftruncate(fd, 0) && len > 0) pwrite(fd, line, (size_t)len, 0);
    fchmod(fd, 0644);
    g_lock_fd = fd;
    syscall(SYS_thr_set_name, -1, CONSOLE_PROC_NAME);
    /* Copies from before the lock don't hold it: find them by name. A copy
     * still deciding hasn't named itself yet, so it's safe from this. */
    kill_by_name();
    return 0;
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

/* Notifications go through the shell, and a request waits until the shell
 * takes it: with the shell hung (it has been, with the HEN's hooks in it), the
 * caller would hang too. So they're handed to a thread of their own, a few
 * at a time; when the shell isn't taking them, the newest are dropped, and
 * nothing else ever waits on it (the mount guard least of all). */
#define NOTIFY_QUEUE 8
static pthread_mutex_t g_notify_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_notify_cond = PTHREAD_COND_INITIALIZER;
static char g_notify_queue[NOTIFY_QUEUE][256];
static int g_notify_head, g_notify_count, g_notify_started;

static void *notify_thread(void *arg) {
    (void)arg;
    for (;;) {
        notify_request_t req;
        memset(&req, 0, sizeof req);
        pthread_mutex_lock(&g_notify_lock);
        while (!g_notify_count) pthread_cond_wait(&g_notify_cond, &g_notify_lock);
        snprintf(req.message, sizeof req.message, "%s", g_notify_queue[g_notify_head]);
        g_notify_head = (g_notify_head + 1) % NOTIFY_QUEUE;
        g_notify_count--;
        pthread_mutex_unlock(&g_notify_lock);
        if (console_toast(req.message) == 0) {
            LOGD("notification shown: %s", req.message);
            continue;
        }
        /* The shell can turn one down; without this a lost notice leaves no trace. */
        const int rc = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
        if (rc) LOGW("notification not shown (0x%08x): %s", (unsigned)rc, req.message);
        else LOGD("notification shown: %s", req.message);
    }
    return NULL;
}

void plat_notify(const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_notify_lock);
    if (!g_notify_started) {
        pthread_t t;
        g_notify_started = pthread_create(&t, NULL, notify_thread, NULL) == 0;
        if (g_notify_started) pthread_detach(t);
    }
    if (g_notify_started && g_notify_count < NOTIFY_QUEUE) {
        snprintf(g_notify_queue[(g_notify_head + g_notify_count) % NOTIFY_QUEUE], sizeof g_notify_queue[0], "%s", msg);
        g_notify_count++;
        pthread_cond_signal(&g_notify_cond);
    }
    pthread_mutex_unlock(&g_notify_lock);
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

int plat_fs_places(char (*places)[PATH_MAX_LEN], int max) {
    /* Internal storage, then whatever drives are plugged in. */
    static const char *const dirs[] = {"/data", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/ext0", "/mnt/ext1"};
    int n = 0;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0] && n < max; i++)
        if (dir_exists(dirs[i])) str_copy(places[n++], PATH_MAX_LEN, dirs[i]);
    return n;
}

const char *plat_name(void) { return plat_info()->name; }

/* libSceSystemService, the same on both consoles (OpenOrbis SystemService.h). */
#define SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME 6
int sceSystemServiceParamGetString(int param_id, char *buf, size_t n);

void plat_console_name(char *buf, size_t n) {
    char name[128] = "";
    buf[0] = 0;
    if (sceSystemServiceParamGetString(SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME, name, sizeof name) != 0) return;
    name[sizeof name - 1] = 0;
    for (const char *c = name; *c; c++)
        if ((unsigned char)*c < 0x20) return; /* not a name */
    snprintf(buf, n, "%s", name);
}

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
