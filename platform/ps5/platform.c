/* platform.h for the PS5 (etaHEN, ps5-payload-dev SDK). The parts shared with
 * the PS4 are in platform/console/console.c.
 *
 * Adapted from John Törnblom's GPLv3 ftpsrv and websrv: removing the old home
 * screen tile (ftpsrv install-ps5.c) and the foreground app (websrv). Links
 * against SceSystemService and SceAppInstUtil. */
#include "platform.h"
#include "emulators_json.h" /* generated from platform/ps5/emulators.json */

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <ps5/kernel.h>

#include "cJSON.h"
#include "console.h"
#include "ps5.h"
#include "util.h" /* APP_VERSION */

/* The "RomM Sync" tile versions before 1.1.1 installed, opening the web UI,
 * and RommPS, which took its place. */
#define OLD_TILE_ID  "ROMM00001"
#define ROMMPS_ID    "PPSA76677"

/* AuthID used by ftpsrv/websrv (ShellCore-like privileges). */
#define ROMM_AUTHID     0x4801000000000013L


/* libkernel: layout from sdk/samples/ps/main.c. Packed: the title starts at
 * byte 16 (unpadded, it would land at 20 and read 4 bytes late). */
typedef struct __attribute__((packed)) app_info {
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
/* pldmgr's autoload starts the copies it keeps here (0.6.3 was seen keeping
 * its own under local/). On a console with neither HEN, that's the only
 * autostart there is. */
static const char *const PAYLOAD_COPIES[] = {"/data/pldmgr/payloads/romm-sync/romm-sync.elf",
                                             "/data/pldmgr/payloads/local/romm-sync/romm-sync.elf", NULL};

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
    .can_mount = 1,
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

int ps5_proc_app(pid_t pid, char title[16], uint32_t *app_id) {
    app_info_t info;
    memset(&info, 0, sizeof info);
    if (sceKernelGetAppInfo(pid, &info)) return -1;
    memcpy(title, info.title_id, sizeof info.title_id);
    title[sizeof info.title_id] = 0;
    if (app_id) *app_id = info.app_id;
    return 0;
}

struct appid_ctx { int app_id; char title_id[16]; };

static int appid_cb(pid_t pid, const char *tdname, void *vctx) {
    struct appid_ctx *c = vctx;
    uint32_t app_id;
    (void)tdname;
    if (ps5_proc_app(pid, c->title_id, &app_id) || (int)app_id != c->app_id) return 0;
    return 1;
}

/* ---- Toasts in the PS5's own style ----
 * sceNotificationSend takes the toast as JSON (the SDK's samples/notify): an
 * icon, a line in bold and one under it. Apps can't load its library (PS5SX2
 * found it refused on 11.40 and 12.00), but a payload from the ELF loader runs
 * in a system process, where it loads. Loaded when first needed, so a console
 * without it still gets the plain notification. */
#define TOAST_USER_SYSTEM 0xFE
/* A notice of one line has a template of its own: the two-line one keeps room
 * for the small line above it, which leaves a lone line sitting low. */
#define TOAST_ONE_LINE_TEMPLATE "InteractiveToastTemplateA"
#define TOAST_ICON "/user/appmeta/" ROMMPS_ID "/icon0.png"
typedef int (*notification_send_fn)(int user_id, bool logged, const char *json);
static notification_send_fn g_toast_send;
static pthread_once_t g_toast_once = PTHREAD_ONCE_INIT;

static void toast_load(void) {
    void *lib = dlopen("libSceNotification.sprx", RTLD_LAZY);
    g_toast_send = lib ? (notification_send_fn)dlsym(lib, "sceNotificationSend") : NULL;
    if (!g_toast_send) LOGW("notifications: libSceNotification isn't there (%s), the plain ones it is", lib ? "no sceNotificationSend" : "dlopen failed");
}

/* RommPS's icon, from the app's home screen copy: the shell reads that one. */
static const char *toast_icon(void) {
    struct stat st;
    return stat(TOAST_ICON, &st) == 0 ? TOAST_ICON : NULL;
}

int console_toast(const char *message) {
    pthread_once(&g_toast_once, toast_load);
    if (!g_toast_send) return -1;
    /* "RommPS: couldn't sync\nCheck your connection": the first line is the
     * point, in bold, without the name the icon already gives; the second is
     * the context, which the toast draws small above it. */
    char text[1024], *sub;
    const char *start = strncmp(message, "RommPS: ", 8) ? message : message + 8;
    str_copy(text, sizeof text, start);
    if ((sub = strchr(text, '\n'))) *sub++ = 0;
    if (text[0] >= 'a' && text[0] <= 'z') text[0] -= 32;
    static unsigned serial;
    char created[32], id[24];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(created, sizeof created, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    snprintf(id, sizeof id, "%u", (unsigned)(ts.tv_sec % 100000000) * 10 + (serial++ % 10));

    cJSON *root = cJSON_CreateObject(), *raw = cJSON_AddObjectToObject(root, "rawData");
    cJSON_AddStringToObject(raw, "viewTemplateType", sub && *sub ? "InteractiveToastTemplateB" : TOAST_ONE_LINE_TEMPLATE);
    cJSON_AddStringToObject(raw, "channelType", "Downloads");
    cJSON_AddStringToObject(raw, "useCaseId", "IDC");
    cJSON_AddStringToObject(raw, "toastOverwriteType", "No");
    cJSON_AddBoolToObject(raw, "isImmediate", 1);
    cJSON_AddNumberToObject(raw, "priority", 100);
    cJSON *view = cJSON_AddObjectToObject(raw, "viewData");
    const char *icon = toast_icon();
    cJSON *i = cJSON_AddObjectToObject(view, "icon");
    if (icon) {
        cJSON_AddStringToObject(i, "type", "Url");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(i, "parameters"), "url", icon);
    } else {
        cJSON_AddStringToObject(i, "type", "Predefined");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(i, "parameters"), "icon", "download");
    }
    cJSON_AddStringToObject(cJSON_AddObjectToObject(view, "message"), "body", text);
    if (sub && *sub) cJSON_AddStringToObject(cJSON_AddObjectToObject(view, "subMessage"), "body", sub);
    /* RommPS installed (its icon is there): a button that opens it, unless
     * it's the app open now. */
    char running[16] = "";
    if (icon && !(plat_running_title(running, sizeof running) == 1 && !strcmp(running, ROMMPS_ID))) {
        cJSON *open = cJSON_CreateObject();
        cJSON_AddStringToObject(open, "actionName", "Open RommPS");
        cJSON_AddStringToObject(open, "actionType", "DeepLink");
        cJSON_AddBoolToObject(open, "defaultFocus", 1);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(open, "parameters"), "actionUrl", "psgm:play?id=" ROMMPS_ID);
        cJSON_AddItemToArray(cJSON_AddArrayToObject(view, "actions"), open);
    }
    /* The view the shell uses when notification previews are off (the SDK's notify sample sends one). */
    cJSON *plain = cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(raw, "platformViews"), "previewDisabled"), "viewData");
    cJSON *plain_icon = cJSON_AddObjectToObject(plain, "icon");
    cJSON_AddStringToObject(plain_icon, "type", "Predefined");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(plain_icon, "parameters"), "icon", "download");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(plain, "message"), "body", text);
    cJSON_AddStringToObject(root, "createdDateTime", created);
    cJSON_AddStringToObject(root, "localNotificationId", id);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const int rc = json ? g_toast_send(TOAST_USER_SYSTEM, true, json) : -1;
    free(json);
    if (rc < 0) LOGW("notifications: sceNotificationSend said 0x%08x", (unsigned)rc);
    return rc < 0 ? -1 : 0;
}

struct alive_ctx { const char *title; int found; };

static int alive_cb(pid_t pid, const char *tdname, void *vctx) {
    struct alive_ctx *c = vctx;
    char title[16];
    (void)tdname;
    if (!ps5_proc_app(pid, title, NULL) && !strncmp(title, c->title, 9)) c->found = 1;
    return c->found;
}

int plat_app_alive(const char *title_id) {
    struct alive_ctx c = {title_id, 0};
    if (!title_id || strlen(title_id) != 9) return -1;
    if (console_for_each_proc(alive_cb, &c) < 0) return -1;
    return c.found;
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
