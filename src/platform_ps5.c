/* platform.h for the PS5 (etaHEN, ps5-payload-dev SDK).
 *
 * Adapted from John Törnblom's GPLv3 ftpsrv and websrv: process lookup and the
 * single-instance check (ftpsrv main-prospero.c), the home screen tile (ftpsrv
 * install-ps5.c), notifications, the LAN address and the foreground app (websrv).
 * Links against SceSystemService and SceAppInstUtil. */
#include "platform.h"

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
#include <strings.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>

#include <ps5/kernel.h>

#define ROMM_PROC_NAME  "romm-sync.elf"
#define ROMM_DATA_DIR   "/data/romm-sync"
#define ROMM_CA_BUNDLE  ROMM_DATA_DIR "/cacert.pem"
#define ROMM_ICON_SRC   ROMM_DATA_DIR "/icon0.png"
#define ROMM_TITLE_ID   "ROMM00001"
#define ROMM_TITLE_NAME "RomM Sync"
#define ROMM_APP_DIR    "/user/app/" ROMM_TITLE_ID
#define ROMM_APPMETA    "/user/appmeta/" ROMM_TITLE_ID "/param.json"

/* AuthID used by ftpsrv/websrv (ShellCore-like privileges). */
#define ROMM_AUTHID     0x4801000000000013L


/* libkernel(_web|_sys): same layout as ftpsrv/websrv notify.c */
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

/* libkernel: layout from sdk/samples/ps/main.c */
typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    uint32_t app_type;
    char     title_id[10];
    char     unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

/* libSceSystemService: websrv sys.c / hbldr.c, etaHEN Discord_RPC.c */
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id, char *title_id);

/* libSceAppInstUtil: ftpsrv install-ps5.c */
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);


static int mkdir_p(const char *path) {
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

/* Visit every process via sysctl(KERN_PROC, KERN_PROC_PROC).
 * Offsets are the ones ftpsrv/websrv use for the PS5 kinfo_proc:
 * ki_structsize @0, ki_pid @72, ki_tdname @447 (TDNAMLEN+1 = 17 bytes).
 * cb returns nonzero to stop; that value is returned. */
typedef int (*proc_cb)(pid_t pid, const char *tdname, void *ctx);

static int for_each_proc(proc_cb cb, void *ctx) {
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
    if (for_each_proc(find_cb, &c) < 0) return -1;
    return c.found;
}

int plat_init(void) {
    pid_t pid;
    int tries = 0;

    signal(SIGPIPE, SIG_IGN);

    /* Name the main thread so find_pid() of the next instance sees us. */
    syscall(SYS_thr_set_name, -1, ROMM_PROC_NAME);

    /* Single instance: kill any older romm-sync.elf (ftpsrv main-prospero.c). */
    while ((pid = find_pid(ROMM_PROC_NAME)) > 0) {
        if (kill(pid, SIGKILL) && errno != ESRCH) break;
        sleep(1);
        if (++tries >= 10) break;
    }

    /* Elevated AuthID as ftpsrv/websrv do; needed for /user/app writes and
     * AppInstUtil. Non-fatal: kernel_* returns -1 without kernel R/W. */
    kernel_set_ucred_authid(getpid(), ROMM_AUTHID);

    if (mkdir_p(ROMM_DATA_DIR)) return -1;
    return 0;
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

static int valid_title_id(const char *s) {
    /* PS5/PS4 title ids: 4 uppercase letters + 5 digits (PPSA01234, CUSA00001) */
    for (int i = 0; i < 9; i++) {
        char ch = s[i];
        if (i < 4 ? !(ch >= 'A' && ch <= 'Z') : !(ch >= '0' && ch <= '9')) return 0;
    }
    return 1;
}

struct appid_ctx { int app_id; char title_id[16]; };

static int appid_cb(pid_t pid, const char *tdname, void *vctx) {
    struct appid_ctx *c = vctx;
    app_info_t info;
    (void)tdname;
    memset(&info, 0, sizeof info);
    if (sceKernelGetAppInfo(pid, &info)) return 0;
    if ((int)info.app_id != c->app_id) return 0;
    memcpy(c->title_id, info.title_id, sizeof info.title_id);
    c->title_id[sizeof info.title_id] = 0;
    return 1;
}

int plat_running_title(char *title_id, size_t n) {
    char tid[256]; /* nobody documents how big this needs to be, so 256 it is */
    int app_id;

    if (!title_id || n < 10) return -1;
    title_id[0] = 0;

    app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) return 0; /* < 0: no BigApp running */

    memset(tid, 0, sizeof tid);
    if (sceSystemServiceGetAppTitleId(app_id, tid) || !valid_title_id(tid)) {
        /* Fallback: find the process owning app_id (sdk samples/ps). */
        struct appid_ctx c;
        memset(&c, 0, sizeof c);
        c.app_id = app_id;
        if (for_each_proc(appid_cb, &c) != 1 || !valid_title_id(c.title_id)) return -1;
        memcpy(tid, c.title_id, sizeof c.title_id);
    }
    tid[9] = 0;
    snprintf(title_id, n, "%s", tid);
    return 1;
}

const char *plat_data_dir(void) {
    mkdir_p(ROMM_DATA_DIR);
    return ROMM_DATA_DIR;
}

const char *plat_ca_bundle(void) {
    struct stat st;
    if (!stat(ROMM_CA_BUNDLE, &st) && st.st_size > 0) return ROMM_CA_BUNDLE;
    return NULL;
}


static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (size && fwrite(data, size, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    return fclose(f) ? -1 : 0;
}

/* Returns malloc'd contents (NUL-terminated) or NULL. */
static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long len;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || len > 16 * 1024 * 1024 ||
        fseek(f, 0, SEEK_SET) || !(buf = malloc((size_t)len + 1)) ||
        (len && fread(buf, (size_t)len, 1, f) != 1)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = 0;
    if (size) *size = (size_t)len;
    return buf;
}

static int copy_file(const char *src, const char *dst) {
    size_t size;
    char *data = read_file(src, &size);
    int r;
    if (!data) return -1;
    r = write_file(dst, data, size);
    free(data);
    return r;
}

static int install_title_dir(const char *title_id, const char *dir) {
    int (*install_dir)(const char *, const char *, void *) = NULL;
    const char *nid = "Wudg3Xe3heE"; /* sceAppInstUtilAppInstallTitleDir */
    uint32_t handle;

    if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
        install_dir = (void *)kernel_dynlib_resolve(-1, handle, nid);
    if (install_dir) return install_dir(title_id, dir, NULL);
    return sceAppInstUtilAppInstallAll(NULL);
}

int plat_install_tile(int port) {
    char param[512];
    struct stat st;
    char *old;
    int installed, have_icon, err;

    if (port <= 0 || port > 65535) return -1;

    snprintf(param, sizeof param,
             "{\n"
             "  \"titleId\": \"" ROMM_TITLE_ID "\",\n"
             "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
             "  \"localizedParameters\": {\n"
             "    \"defaultLanguage\": \"en-US\",\n"
             "    \"en-US\": {\n"
             "      \"titleName\": \"" ROMM_TITLE_NAME "\"\n"
             "    }\n"
             "  }\n"
             "}\n",
             port);

    installed = !stat(ROMM_APPMETA, &st);
    have_icon = !stat(ROMM_ICON_SRC, &st);

    /* Already installed with identical param.json (same port) -> nothing to do. */
    if (installed && (old = read_file(ROMM_APP_DIR "/sce_sys/param.json", NULL))) {
        int same = !strcmp(old, param);
        free(old);
        if (same && (!have_icon || !stat(ROMM_APP_DIR "/sce_sys/icon0.png", &st)))
            return 0;
    }

    if ((err = sceAppInstUtilInitialize())) return -1;

    /* Re-install (e.g. port changed): uninstall may remove /user/app/<id>. */
    if (installed) sceAppInstUtilAppUnInstall(ROMM_TITLE_ID);

    if (mkdir_p(ROMM_APP_DIR "/sce_sys")) return -1;
    if (write_file(ROMM_APP_DIR "/sce_sys/param.json", param, strlen(param))) return -1;
    if (have_icon) copy_file(ROMM_ICON_SRC, ROMM_APP_DIR "/sce_sys/icon0.png");

    if ((err = install_title_dir(ROMM_TITLE_ID, "/user/app/"))) return -1;
    return 0;
}

const char *plat_name(void) {
    return "ps5";
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
