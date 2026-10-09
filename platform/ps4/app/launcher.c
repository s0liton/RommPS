/* RomM Sync for PS4: the home screen app. The sync itself is the payload,
 * which GoldHEN runs from /data/payloads (platform/ps4/platform.c); this app
 * only installs that payload and shows its web UI, so it never needs to stay
 * running. Built with OpenOrbis (platform/ps4/ps4.mk, "make ps4-pkg").
 *
 * When opened it:
 *   1. copies the payload bundled in the package to /data/payloads when it's
 *      missing or older than the bundled one. Versions come from the
 *      ROMM_SYNC_VERSION= tag in each payload (src/main.c), so an older app
 *      never undoes a newer payload from an update or a manual copy. This app
 *      runs as a regular user, so it only relies on /data/payloads being
 *      writable. Files are read in full: stat() is unreliable in the sandbox;
 *   2. checks that RomM Sync answers on its web port;
 *   3. shows the web UI in the system web browser dialog, or the Browser app
 *      if the dialog won't open; if RomM Sync isn't running, says how to start it.
 *
 * Each step goes to /data/romm-sync/launcher.log, for bug reports.
 *
 * The web browser dialog has no OpenOrbis types; its layout is Sony's
 * SceWebBrowserDialogParam as used by Nuvio-PS5 and EVO Player. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define BUNDLED_PAYLOAD "/app0/romm-sync-ps4.elf"
#define PAYLOAD_DIR     "/data/payloads"
#define PAYLOAD_PATH    PAYLOAD_DIR "/romm-sync-ps4.elf"
#define DATA_DIR        "/data/romm-sync"
#define LOG_PATH        DATA_DIR "/launcher.log"
#define CONFIG_PATH     DATA_DIR "/config.json"
#define VERSION_TAG     "ROMM_SYNC_VERSION="
#define DEFAULT_PORT    8780

/* libSceWebBrowserDialog */
typedef struct {
    OrbisCommonDialogBaseParam base;
    uint64_t size;
    int32_t mode; /* 1: the browser's own layout, 2: the rectangle below */
    int32_t user_id;
    const char *url;
    void *callback_init;
    uint16_t width, height, pos_x, pos_y;
    uint32_t parts;
    uint16_t header_width, header_x, header_y, pad0;
    uint32_t control;
    void *ime_param;
    void *webview_param;
    uint32_t animation;
    uint8_t reserved[202];
    uint16_t tail_pad;
} web_dialog_param;
_Static_assert(sizeof(web_dialog_param) == 328, "SceWebBrowserDialogParam is 328 bytes");

int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(web_dialog_param *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogClose(void);
int sceWebBrowserDialogTerminate(void);
/* OpenOrbis declares it without parameters; this is the real signature. */
int launch_web_browser(const char *uri, void *param) __asm__("sceSystemServiceLaunchWebBrowser");

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    char ts[32];
    time_t now = time(NULL);
    va_list ap;
    if (!f) return;
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int user_id(void) {
    int id = -1;
    if (sceUserServiceGetForegroundUser(&id) == 0 && id != -1 && id != 0xff) return id;
    if (sceUserServiceGetInitialUser(&id) == 0) return id;
    return -1;
}

/* Whole file, by reading: stat() sizes can't be trusted in the sandbox. */
static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    size_t cap = 0, n = 0, r;
    if (!f) return NULL;
    do {
        if (n == cap) {
            unsigned char *nb = realloc(buf, cap = cap ? cap * 2 : 1 << 20);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        r = fread(buf + n, 1, cap - n, f);
        n += r;
    } while (r > 0);
    fclose(f);
    *len = n;
    return buf;
}

/* The version in a payload's ROMM_SYNC_VERSION= tag, or "" if it has none
 * (payloads older than the tag). */
static void payload_tag(const unsigned char *elf, size_t len, char *out, size_t n) {
    size_t tl = strlen(VERSION_TAG);
    out[0] = 0;
    for (size_t i = 0; i + tl < len; i++) {
        if (elf[i] != 'R' || memcmp(elf + i, VERSION_TAG, tl) != 0) continue;
        size_t j = 0;
        for (const unsigned char *v = elf + i + tl; v < elf + len && *v != '\n' && *v && j + 1 < n; v++) out[j++] = (char)*v;
        out[j] = 0;
        return;
    }
}

/* "1.2.3" or "1.2.3-ps4-prototype.6": the three numbers, and the last number
 * of the pre-release part (-1 for a release). 0 if it isn't a version. */
static int parse_version(const char *s, int v[4]) {
    v[3] = -1;
    if (sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) != 3) return 0;
    const char *pre = strchr(s, '-');
    if (pre) {
        v[3] = 0;
        for (const char *c = pre; *c; c++)
            if (*c >= '0' && *c <= '9' && (c == pre || c[-1] < '0' || c[-1] > '9')) v[3] = atoi(c);
    }
    return 1;
}

/* >0 if a is newer than b. A release is newer than its pre-releases. */
static int compare_versions(const int a[4], const int b[4]) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] - b[i];
    if ((a[3] < 0) != (b[3] < 0)) return a[3] < 0 ? 1 : -1;
    return a[3] - b[3];
}

/* 1 if installed or replaced, 0 if left as it is, -1 on failure. */
static int install_payload(void) {
    size_t want_len = 0, have_len = 0;
    unsigned char *want = slurp(BUNDLED_PAYLOAD, &want_len), *have;
    char want_ver[64], have_ver[64];
    int wv[4], hv[4], rc = -1;
    if (!want || want_len == 0) {
        logf_("can't read the bundled payload %s", BUNDLED_PAYLOAD);
        free(want);
        return -1;
    }
    payload_tag(want, want_len, want_ver, sizeof want_ver);
    have = slurp(PAYLOAD_PATH, &have_len);
    if (have) payload_tag(have, have_len, have_ver, sizeof have_ver);
    else have_ver[0] = 0;
    int replace;
    if (!have) replace = 1;
    else if (have_len == want_len && !memcmp(have, want, want_len)) replace = 0;
    else if (!parse_version(want_ver, wv)) replace = 0; /* an untagged test build: never overwrite */
    else if (!parse_version(have_ver, hv)) replace = 1; /* older than version tags */
    else replace = compare_versions(wv, hv) > 0;
    if (!replace) {
        logf_("payload %s left as it is (installed %s, this app has %s)", PAYLOAD_PATH, have_ver[0] ? have_ver : "?",
              want_ver[0] ? want_ver : "?");
        rc = 0;
    } else {
        FILE *f;
        mkdir(PAYLOAD_DIR, 0777);
        if ((f = fopen(PAYLOAD_PATH ".tmp", "wb"))) {
            int ok = fwrite(want, 1, want_len, f) == want_len;
            if (fclose(f) == 0 && ok) rc = rename(PAYLOAD_PATH ".tmp", PAYLOAD_PATH) == 0 ? 1 : -1;
        }
        logf_("%s %s with %s (was %s): %s", have ? "replaced" : "installed", PAYLOAD_PATH,
              want_ver[0] ? want_ver : "?", have ? (have_ver[0] ? have_ver : "untagged") : "missing",
              rc == 1 ? "ok" : "failed");
    }
    free(want);
    free(have);
    return rc;
}

/* The web_port from RomM Sync's config.json, if it was changed. */
static int web_port(void) {
    size_t len = 0;
    char *cfg = (char *)slurp(CONFIG_PATH, &len), *p;
    int port = DEFAULT_PORT;
    if (cfg) {
        cfg = realloc(cfg, len + 1);
        cfg[len] = 0;
        if ((p = strstr(cfg, "\"web_port\"")) && (p = strchr(p, ':'))) port = atoi(p + 1);
        free(cfg);
    }
    return port > 0 && port < 65536 ? port : DEFAULT_PORT;
}

/* The running payload's version from /api/status, or 0 if nothing answers. */
static int payload_version(int port, char *version, size_t n) {
    struct sockaddr_in sa;
    char buf[2048], req[128], *v;
    int fd, got = 0, r;
    version[0] = 0;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 0;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return 0;
    }
    snprintf(req, sizeof req, "GET /api/status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
    write(fd, req, strlen(req));
    while (got < (int)sizeof buf - 1 && (r = (int)read(fd, buf + got, sizeof buf - 1 - got)) > 0) got += r;
    close(fd);
    buf[got] = 0;
    if ((v = strstr(buf, "\"version\":\"")) || (v = strstr(buf, "\"version\": \""))) {
        v = strchr(v + 9, '"') + 1;
        snprintf(version, n, "%.*s", (int)strcspn(v, "\""), v);
    }
    return 1;
}

static void message(const char *text) {
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam user;
    memset(&param, 0, sizeof param);
    memset(&user, 0, sizeof user);
    param.baseParam.size = sizeof param.baseParam;
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&param.baseParam);
    param.size = sizeof param;
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    param.userId = user_id();
    user.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = text;
    if (sceMsgDialogInitialize() < 0 && sceMsgDialogGetStatus() == ORBIS_COMMON_DIALOG_STATUS_NONE) {
        logf_("message dialog unavailable: %s", text);
        return;
    }
    if (sceMsgDialogOpen(&param) < 0) {
        logf_("message dialog didn't open: %s", text);
        return;
    }
    while (sceMsgDialogUpdateStatus() == ORBIS_COMMON_DIALOG_STATUS_RUNNING) usleep(16 * 1000);
    sceMsgDialogTerminate();
}

/* The dialog's magic encodes this block's address, so it must not move. */
static web_dialog_param g_web __attribute__((aligned(16)));

static int open_web_dialog(const char *url, int mode) {
    memset(&g_web, 0, sizeof g_web);
    g_web.base.size = sizeof g_web.base;
    g_web.base.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&g_web.base);
    g_web.size = sizeof g_web;
    g_web.mode = mode;
    g_web.user_id = user_id();
    g_web.url = url;
    if (mode == 2) {
        g_web.width = 1920;
        g_web.height = 1080;
        g_web.header_width = 1920;
    }
    return sceWebBrowserDialogOpen(&g_web);
}

/* Full screen, then the browser's own layout, then the Browser app. Returns
 * when the dialog is closed, or straight away for the Browser app. */
static void show_web_ui(const char *url) {
    int rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_WEB_BROWSER_DIALOG);
    logf_("web browser dialog module: 0x%08x", (unsigned)rc);
    rc = sceWebBrowserDialogInitialize();
    logf_("web browser dialog init: 0x%08x", (unsigned)rc);
    if ((rc = open_web_dialog(url, 2)) != 0) {
        logf_("full-screen web dialog: 0x%08x, trying the default layout", (unsigned)rc);
        rc = open_web_dialog(url, 1);
    }
    if (rc == 0) {
        logf_("web dialog open on %s", url);
        int status;
        while ((status = sceWebBrowserDialogUpdateStatus()) == ORBIS_COMMON_DIALOG_STATUS_RUNNING ||
               status == ORBIS_COMMON_DIALOG_STATUS_INITIALIZED)
            usleep(16 * 1000);
        logf_("web dialog closed (status %d)", status);
        sceWebBrowserDialogClose();
        sceWebBrowserDialogTerminate();
        return;
    }
    logf_("web dialog: 0x%08x, opening the Browser app instead", (unsigned)rc);
    rc = launch_web_browser(url, NULL);
    logf_("Browser app: 0x%08x", (unsigned)rc);
    if (rc != 0) {
        char text[256];
        snprintf(text, sizeof text, "Open %s in the web browser to use RomM Sync.", url);
        message(text);
    }
}

int main(void) {
    char url[64], running[32], text[512];
    int installed, port;

    sceSystemServiceHideSplashScreen();
    mkdir(DATA_DIR, 0777);
    logf_("RomM Sync launcher %s", APP_VERSION);
    sceUserServiceInitialize(NULL);
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
    sceCommonDialogInitialize();

    installed = install_payload();
    port = web_port();
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);
    int up = payload_version(port, running, sizeof running);
    logf_("RomM Sync on port %d: %s%s", port, up ? "running " : "not running", running);

    if (installed < 0) {
        message("RomM Sync couldn't copy its payload to /data/payloads. Copy romm-sync-ps4.elf there yourself over FTP, then run it from GoldHEN Settings > Payloader.");
    } else if (!up) {
        snprintf(text, sizeof text,
                 "RomM Sync isn't running.\n\n%sIn GoldHEN Settings > Payloader, run romm-sync-ps4.elf, then add it to the "
                 "AutoRun queue so it starts on every jailbreak. Open this app again afterwards.",
                 installed ? "Its payload is now in /data/payloads. " : "");
        message(text);
    } else {
        if (installed)
            message("A new version of RomM Sync was copied to /data/payloads. It takes over after the next jailbreak; "
                    "until then the running version stays in use.");
        show_web_ui(url);
    }

    logf_("launcher done");
    sceSystemServiceLoadExec("exit", NULL);
    return 0;
}
