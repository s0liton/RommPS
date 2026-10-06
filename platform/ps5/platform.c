/* platform.h for the PS5 (etaHEN, ps5-payload-dev SDK). The parts shared with
 * the PS4 are in platform/console/console.c.
 *
 * Adapted from John Törnblom's GPLv3 ftpsrv and websrv: the home screen tile
 * (ftpsrv install-ps5.c) and the foreground app (websrv). Links against
 * SceSystemService and SceAppInstUtil. */
#include "platform.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <ps5/kernel.h>

#include "console.h"

#define ROMM_TITLE_ID   "ROMM00001"
#define ROMM_TITLE_NAME "RomM Sync"
#define ROMM_APP_DIR    "/user/app/" ROMM_TITLE_ID
#define ROMM_APPMETA    "/user/appmeta/" ROMM_TITLE_ID "/param.json"

/* AuthID used by ftpsrv/websrv (ShellCore-like privileges). */
#define ROMM_AUTHID     0x4801000000000013L


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


static const char *const HOMEBREW_DIRS[] = {
    "/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
    "/mnt/usb3/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", NULL};
static const char *const EMULATOR_DIRS[] = {NULL};

static const plat_info_t INFO = {
    .name = "ps5",
    .console = "PlayStation 5",
    .client = "romm-sync-ps5",
    .loader = "etaHEN",
    .payload = "romm-sync.elf",
    .manifest = "manifest.json",
    .manifest_sig = "manifest.sig",
    /* etaHEN starts each payloads/<name>.elf that has a <name>.elf.auto_start next to it. */
    .autostart_dir = "/data/etaHEN/payloads",
    .autostart_flag = "romm-sync.elf.auto_start",
    .loader_dir = "/data/etaHEN",
    .homebrew_dirs = HOMEBREW_DIRS,
    .emulator_dirs = EMULATOR_DIRS,
    .retroarch_root = "/data/homebrew/PPSA99169",
    .mednafen_root = "/data/homebrew/Mednafen",
    .has_tile = 1,
    .can_relaunch = 1,
};

const plat_info_t *plat_info(void) { return &INFO; }


int plat_init(void) {
    signal(SIGPIPE, SIG_IGN);
    console_single_instance();

    /* Elevated AuthID as ftpsrv/websrv do; needed for /user/app writes and
     * AppInstUtil. Non-fatal: kernel_* returns -1 without kernel R/W. */
    kernel_set_ucred_authid(getpid(), ROMM_AUTHID);

    if (console_mkdir_p(CONSOLE_DATA_DIR)) return -1;
    return 0;
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
    if (sceSystemServiceGetAppTitleId(app_id, tid) || !console_valid_title_id(tid)) {
        /* Fallback: find the process owning app_id (sdk samples/ps). */
        struct appid_ctx c;
        memset(&c, 0, sizeof c);
        c.app_id = app_id;
        if (console_for_each_proc(appid_cb, &c) != 1 || !console_valid_title_id(c.title_id)) return -1;
        memcpy(tid, c.title_id, sizeof c.title_id);
    }
    tid[9] = 0;
    snprintf(title_id, n, "%s", tid);
    return 1;
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

void plat_describe(char *buf, size_t n) {
    app_info_t info;
    char host[16] = "";
    memset(&info, 0, sizeof info);
    if (!sceKernelGetAppInfo(getpid(), &info)) snprintf(host, sizeof host, "%.9s", info.title_id);
    console_describe(buf, n, host);
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
    have_icon = !stat(CONSOLE_ICON, &st);

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

    if (console_mkdir_p(ROMM_APP_DIR "/sce_sys")) return -1;
    if (write_file(ROMM_APP_DIR "/sce_sys/param.json", param, strlen(param))) return -1;
    if (have_icon) copy_file(CONSOLE_ICON, ROMM_APP_DIR "/sce_sys/icon0.png");

    if ((err = install_title_dir(ROMM_TITLE_ID, "/user/app/"))) return -1;
    return 0;
}

/* etaHEN's loader runs whatever arrives on port 9021, loopback included
 * (checked on 12.70 with etaHEN). */
int plat_launch_elf(const void *elf, size_t len) { return console_send_elf(9021, elf, len); }
