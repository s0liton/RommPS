/* platform.h for the PS5 (etaHEN, ps5-payload-dev SDK). The parts shared with
 * the PS4 are in platform/console/console.c.
 *
 * Adapted from John Törnblom's GPLv3 ftpsrv and websrv: removing the old home
 * screen tile (ftpsrv install-ps5.c) and the foreground app (websrv). Links
 * against SceSystemService and SceAppInstUtil. */
#include "platform.h"
#include "emulators_json.h" /* generated from platform/ps5/emulators.json */

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <ps5/kernel.h>

#include "console.h"
#include "util.h" /* APP_VERSION */

/* The "RomM Sync" tile versions before 1.1.1 installed, opening the web UI,
 * and RommPS, which took its place. */
#define OLD_TILE_ID  "ROMM00001"
#define ROMMPS_ID    "PPSA76677"

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
int sceAppInstUtilAppUnInstall(const char *);


static const char *const HOMEBREW_DIRS[] = {
    "/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
    "/mnt/usb3/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", NULL};
static const char *const EMULATOR_DIRS[] = {NULL};

/* Two HENs are in use, with the same autostart convention in their own
 * folders: each starts payloads/<name>.elf when <name>.elf.auto_start sits
 * next to it. onionHEN (github.com/aydencharles/onionHEN) keeps its runtime
 * state in /system_tmp/onionhen, which is gone after a reboot, so that tells
 * the one running now; they refuse to run together. Both leave the ELF loader
 * on 9021 to us. Autostart is set up in both HENs' folders, so RomM Sync
 * starts with either, and switching HENs never brings back an old copy. */
static const char *const ETAHEN_ALSO[] = {"/data/OnionHEN/payloads", NULL};
static const char *const ONIONHEN_ALSO[] = {"/data/etaHEN/payloads", NULL};
/* pldmgr's autoload starts the copies it keeps here. */
static const char *const PAYLOAD_COPIES[] = {"/data/pldmgr/payloads/romm-sync/romm-sync.elf", NULL};

static plat_info_t INFO = {
    .name = "ps5",
    .console = "PlayStation 5",
    .client = "romm-sync-ps5",
    .loader = "etaHEN",
    .payload = "romm-sync.elf",
    .manifest = "manifest.json",
    .manifest_sig = "manifest.sig",
    .autostart_dir = "/data/etaHEN/payloads",
    .autostart_flag = "romm-sync.elf.auto_start",
    .autostart_also = ETAHEN_ALSO,
    .payload_copies = PAYLOAD_COPIES,
    .loader_dir = "/data/etaHEN",
    .homebrew_dirs = HOMEBREW_DIRS,
    .emulator_dirs = EMULATOR_DIRS,
    .emulator_catalog = EMULATORS_JSON,
    .retroarch_root = "/data/homebrew/PPSA99169",
    .mednafen_root = "/data/homebrew/Mednafen",
    .can_relaunch = 1,
    .self_update = 0,
};

static int is_dir(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* onionHEN if it's running, else etaHEN if it's installed, else whichever is. */
static void detect_hen(void) {
    int onion = is_dir("/system_tmp/onionhen") || (!is_dir("/data/etaHEN") && is_dir("/data/OnionHEN"));
    if (!onion) return;
    INFO.loader = "onionHEN";
    INFO.autostart_dir = "/data/OnionHEN/payloads";
    INFO.autostart_also = ONIONHEN_ALSO;
    INFO.loader_dir = "/data/OnionHEN";
}

const plat_info_t *plat_info(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, detect_hen);
    return &INFO;
}


int plat_init(void) {
    signal(SIGPIPE, SIG_IGN);

    /* Elevated AuthID as ftpsrv/websrv do; needed for AppInstUtil.
     * Non-fatal: kernel_* returns -1 without kernel R/W. */
    kernel_set_ucred_authid(getpid(), ROMM_AUTHID);

    if (console_single_instance(APP_VERSION)) return 1;
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


void plat_describe(char *buf, size_t n) {
    app_info_t info;
    char host[16] = "";
    memset(&info, 0, sizeof info);
    if (!sceKernelGetAppInfo(getpid(), &info)) snprintf(host, sizeof host, "%.9s", info.title_id);
    console_describe(buf, n, host);
}

static int on_home_screen(const char *title_id) {
    char app[64], meta[64];
    snprintf(app, sizeof app, "/user/app/%s", title_id);
    snprintf(meta, sizeof meta, "/user/appmeta/%s", title_id);
    return dir_exists(app) || dir_exists(meta);
}

int plat_app_installed(void) { return on_home_screen(ROMMPS_ID); }

/* From its param.json: contentVersion "01.001.000" is 1.1.0. The app's own
 * folder first (mount.lnk names it when ShadowMount mounts it from
 * /data/homebrew): copying a new version over it leaves the home screen's
 * copies in /user/app and /user/appmeta at the old one. */
void plat_app_version(char *out, size_t n) {
    char params[3][PATH_MAX_LEN] = {"", "/user/app/" ROMMPS_ID "/sce_sys/param.json", "/user/appmeta/" ROMMPS_ID "/param.json"};
    char *link = read_file("/user/app/" ROMMPS_ID "/mount.lnk", NULL);
    if (link) {
        link[strcspn(link, "\r\n")] = 0;
        if (link[0] == '/') snprintf(params[0], sizeof params[0], "%s/sce_sys/param.json", link);
        free(link);
    }
    out[0] = 0;
    for (size_t i = 0; i < 3 && !out[0]; i++) {
        if (!params[i][0]) continue;
        char *json = read_file(params[i], NULL);
        const char *k = json ? strstr(json, "\"contentVersion\"") : NULL;
        const char *q = k ? strchr(k + 16, '"') : NULL;
        int v[3];
        if (q && sscanf(q + 1, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3) snprintf(out, n, "%d.%d.%d", v[0], v[1], v[2]);
        free(json);
    }
}

void plat_remove_old_tile(void) {
    if (!on_home_screen(OLD_TILE_ID)) return;
    /* Without RommPS the tile is the only way in from the home screen. */
    if (!on_home_screen(ROMMPS_ID)) {
        LOGI("kept the old RomM Sync tile: RommPS isn't installed");
        return;
    }
    int err = sceAppInstUtilInitialize();
    if (!err) err = sceAppInstUtilAppUnInstall(OLD_TILE_ID);
    if (err) LOGW("could not remove the old RomM Sync tile (0x%x)", (unsigned)err);
    else LOGI("removed the old RomM Sync tile: RommPS is the app now");
}

/* etaHEN's loader runs whatever arrives on port 9021, loopback included
 * (checked on 12.70 with etaHEN). */
int plat_launch_elf(const void *elf, size_t len) { return console_send_elf(9021, elf, len); }
