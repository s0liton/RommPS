/* platform.h for the PS4 (GoldHEN, ps4-payload-dev SDK). The parts shared with
 * the PS5 are in platform/console/console.c.
 *
 * GoldHEN (2.4b18.5 or newer) runs romm-sync-ps4.elf from its payload menu,
 * which reads /data/payloads and can AutoRun it on every jailbreak. The payload
 * runs inside a GoldHEN system process (NPXS21002 on 2.4b18.9), as root, so
 * main() hands that thread back (plat_info detach) and an older copy is never
 * killed: it would take the host process with it. A lock file keeps it to one.
 * The SDK's startup code escapes the sandbox, so there's nothing to raise here.
 *
 * The foreground app follows the SDK's samples/ps. Links against SceSystemService. */
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>

#include "console.h"
#include "emulators_json.h" /* generated from platform/ps4/emulators.json */

#define LOCK_FILE CONSOLE_LOCK_FILE


/* libkernel: layout from sdk/samples/ps/main.c (title_id is wider than the PS5's) */
typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char     title_id[14];
    char     unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

/* libSceSystemService: no GetAppIdOfRunningBigApp or GetAppTitleId on the PS4 */
int sceSystemServiceGetAppIdOfBigApp(void);


static const char *const HOMEBREW_DIRS[] = {
    "/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", NULL};
/* RetroArch keeps its config, saves and core .info files here; the cores
 * themselves are in /data/self/retroarch/cores (see retroarch.cfg). */
static const char *const EMULATOR_DIRS[] = {"/data/retroarch", NULL};

static const plat_info_t INFO = {
    .name = "ps4",
    .console = "PlayStation 4",
    .client = "romm-sync-ps4",
    .loader = "GoldHEN",
    .payload = "romm-sync-ps4.elf",
    .manifest = "manifest-ps4.json",
    .manifest_sig = "manifest-ps4.sig",
    /* GoldHEN's payload menu lists this folder; the user turns on AutoRun
     * for the payload there once (the queue is the menu's own). */
    .autostart_dir = "/data/payloads",
    .autostart_flag = NULL,
    .autostart_hint = "Turn on AutoRun for it in GoldHEN Settings, Payloader, AutoRun queue.",
    .loader_dir = "/data/GoldHEN",
    .homebrew_dirs = HOMEBREW_DIRS,
    .emulator_dirs = EMULATOR_DIRS,
    .retroarch_root = "/data/retroarch",
    .mednafen_root = "/data/mednafen",
    .emulator_catalog = EMULATORS_JSON,
    /* GoldHEN runs the payload on a thread of its own process. */
    .detach = 1,
    .self_update = 1,
};

const plat_info_t *plat_info(void) { return &INFO; }


/* Startup steps go to <data>/boot.log before they run, so a payload that
 * hangs or dies inside the loader still says how far it got. */
static void boot_step(const char *fmt, ...) {
    FILE *f = fopen(CONSOLE_DATA_DIR "/boot.log", "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Held for as long as this copy runs. flock is per open file, so a second copy
 * in the same process (the usual case under GoldHEN) is refused too. */
static int g_lock_fd = -1;

int plat_init(void) {
    int rc = console_mkdir_p(CONSOLE_DATA_DIR);
    /* The home screen app runs as a regular user and logs here. */
    chmod(CONSOLE_DATA_DIR, 0777);
    unlink(CONSOLE_DATA_DIR "/boot.log");
    boot_step("start: pid %d uid %d", getpid(), getuid());
    g_lock_fd = open(LOCK_FILE, O_RDWR | O_CREAT, 0666);
    if (g_lock_fd >= 0 && flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        boot_step("already running");
        close(g_lock_fd);
        g_lock_fd = -1;
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    boot_step("platform ready");
    return rc ? -1 : 0;
}

struct appid_ctx { int app_id; char title_id[16]; };

static int appid_cb(pid_t pid, const char *tdname, void *vctx) {
    struct appid_ctx *c = vctx;
    app_info_t info;
    (void)tdname;
    memset(&info, 0, sizeof info);
    if (sceKernelGetAppInfo(pid, &info)) return 0;
    if ((int)info.app_id != c->app_id) return 0;
    memcpy(c->title_id, info.title_id, 9);
    c->title_id[9] = 0;
    return 1;
}

/* The big app's id, then the process that carries it (sdk samples/ps). */
int plat_running_title(char *title_id, size_t n) {
    struct appid_ctx c;

    if (!title_id || n < 10) return -1;
    title_id[0] = 0;

    memset(&c, 0, sizeof c);
    c.app_id = sceSystemServiceGetAppIdOfBigApp();
    if (c.app_id <= 0) return 0; /* < 0: no big app running */
    if (console_for_each_proc(appid_cb, &c) != 1 || !console_valid_title_id(c.title_id)) return -1;
    snprintf(title_id, n, "%s", c.title_id);
    return 1;
}

void plat_describe(char *buf, size_t n) {
    app_info_t info;
    char host[16] = "";
    memset(&info, 0, sizeof info);
    if (!sceKernelGetAppInfo(getpid(), &info)) snprintf(host, sizeof host, "%.9s", info.title_id);
    console_describe(buf, n, host);
}

void plat_remove_old_tile(void) {}

int plat_app_installed(void) { return 0; }

void plat_app_version(char *out, size_t n) {
    if (n) out[0] = 0;
}

/* Not used (can_relaunch is 0): GoldHEN's network loader (9090) crashes on
 * ELFs in 2.4b18.x, so a new version starts with the next jailbreak. */
int plat_launch_elf(const void *elf, size_t len) {
    (void)elf;
    (void)len;
    errno = ENOTSUP;
    return -1;
}
