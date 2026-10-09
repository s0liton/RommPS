/* Updates from GitHub releases. Nothing installs by itself: the UI checks,
 * shows the release notes and installs when the user asks.
 *
 * A release carries a payload, manifest and signature for each console, named in
 * plat_info(): romm-sync.elf, manifest.json and manifest.sig for the PS5,
 * romm-sync-ps4.elf, manifest-ps4.json and manifest-ps4.sig for the PS4. The
 * manifest is {version, file, size, sha512} and the signature is Ed25519 by one
 * of the keys in update_keys.h. The payload is only installed if all of that
 * checks out, including the manifest naming this console's payload.
 * tools/release-sign.c makes the manifest and signature in CI.
 *
 * ROMM_SYNC_UPDATE_URL points the check somewhere else, for tests. */
#include "update.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "autostart.h"
#include "config.h"
#include "http.h"
#include "library.h"
#include "monocypher-ed25519.h"
#include "platform.h"
#include "state.h"
#include "sync.h"
#include "update_keys.h"
#include "util.h"

#ifndef RELEASES_URL /* overridable for test builds */
#define RELEASES_URL "https://api.github.com/repos/s0liton/RommPS/releases/latest"
#endif
#define CHECK_EVERY_SEC (24 * 60 * 60)
/* A failed daily check (no network yet after boot, say) is retried sooner. */
#define RETRY_AFTER_SEC (60 * 60)
/* The new copy stops this one within a few seconds of starting. */
#define RESTART_TIMEOUT_SEC 60

/* U_PENDING: installed, but this console can't restart into it (PS4); it runs
 * from the next jailbreak. */
typedef enum { U_IDLE, U_CHECKING, U_DOWNLOADING, U_WAITING, U_INSTALLING, U_RESTARTING, U_ERROR, U_PENDING } ustate;
static const char *const STATE_NAMES[] = {"idle", "checking", "downloading", "waiting", "installing", "restarting", "error", "pending"};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ustate g_state;
static char g_error[256], g_message[128];
static char g_latest[32], g_published[32], g_page_url[512];
static char g_elf_url[512], g_manifest_url[512], g_sig_url[512];
static char *g_notes;
static time_t g_checked_at, g_auto_checked_at;
static char g_notified[32], g_pending[32];
static int64_t g_done, g_total;

static const char *release_url(void) {
    const char *u = getenv("ROMM_SYNC_UPDATE_URL");
    return u && *u ? u : RELEASES_URL;
}

/* "v1.2.3" or "1.2.3-dev" into numbers. Anything after the patch is ignored. */
static int parse_version(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v') s++;
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 2 ? 0 : -1;
}

static int newer_than(const char *a, const char *b) {
    int x[3], y[3];
    if (parse_version(a, x) || parse_version(b, y)) return 0;
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return 0;
}

static int busy(ustate s) { return s != U_IDLE && s != U_ERROR && s != U_PENDING; }

/* Call with g_lock held. */
static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
    g_state = U_ERROR;
    g_message[0] = 0;
    LOGE("update: %s", g_error);
}

static void set_message(ustate st, const char *msg) {
    pthread_mutex_lock(&g_lock);
    g_state = st;
    str_copy(g_message, sizeof g_message, msg);
    pthread_mutex_unlock(&g_lock);
}

static void asset_url(const cJSON *assets, const char *name, char *out, size_t n) {
    const cJSON *a;
    out[0] = 0;
    cJSON_ArrayForEach(a, assets) {
        if (!strcmp(jget_str(a, "name", ""), name)) str_copy(out, n, jget_str(a, "browser_download_url", ""));
    }
}

static void *check_thread(void *arg) {
    int notify = (int)(long)arg;
    http_resp r = {0};
    cJSON *j = NULL;
    char err[256] = "";
    if (http_get_verified(release_url(), &r) != 0) snprintf(err, sizeof err, "can't reach GitHub: %s", r.error);
    else if (r.status == 404) snprintf(err, sizeof err, "no releases published yet");
    else if (r.status != 200) snprintf(err, sizeof err, "GitHub answered with HTTP %ld", r.status);
    else if (!(j = cJSON_Parse(r.body)) || !jget_str(j, "tag_name", "")[0]) snprintf(err, sizeof err, "unexpected reply from GitHub");
    http_resp_free(&r);

    pthread_mutex_lock(&g_lock);
    g_checked_at = time(NULL);
    if (err[0]) {
        set_error("%s", err);
    } else {
        const cJSON *assets = cJSON_GetObjectItemCaseSensitive(j, "assets");
        const char *tag = jget_str(j, "tag_name", "");
        str_copy(g_latest, sizeof g_latest, tag[0] == 'v' ? tag + 1 : tag);
        str_copy(g_published, sizeof g_published, jget_str(j, "published_at", ""));
        str_copy(g_page_url, sizeof g_page_url, jget_str(j, "html_url", ""));
        asset_url(assets, plat_info()->payload, g_elf_url, sizeof g_elf_url);
        asset_url(assets, plat_info()->manifest, g_manifest_url, sizeof g_manifest_url);
        asset_url(assets, plat_info()->manifest_sig, g_sig_url, sizeof g_sig_url);
        free(g_notes);
        g_notes = strdup(jget_str(j, "body", ""));
        g_state = U_IDLE;
        g_error[0] = g_message[0] = 0;
        int available = newer_than(g_latest, APP_VERSION);
        LOGI("update check: latest release %s, running %s", g_latest, APP_VERSION);
        if (notify && available && strcmp(g_notified, g_latest) != 0) {
            str_copy(g_notified, sizeof g_notified, g_latest);
            plat_notify("RomM Sync %s is available. Update it from Settings.", g_latest);
        }
    }
    pthread_mutex_unlock(&g_lock);
    cJSON_Delete(j);
    return NULL;
}

int update_check(int notify) {
    pthread_mutex_lock(&g_lock);
    if (busy(g_state)) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_state = U_CHECKING;
    g_message[0] = 0;
    pthread_mutex_unlock(&g_lock);
    if (thread_start(check_thread, (void *)(long)notify) != 0) {
        pthread_mutex_lock(&g_lock);
        set_error("cannot start the update check");
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    return 0;
}

void update_tick(void) {
    config_lock();
    int on = g_cfg.update_check;
    config_unlock();
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    time_t every = g_state == U_ERROR ? RETRY_AFTER_SEC : CHECK_EVERY_SEC;
    pthread_mutex_unlock(&g_lock);
    if (!on || now - g_auto_checked_at < every) return;
    g_auto_checked_at = now;
    update_check(1);
}

/* The manifest must carry a valid signature by a trusted key, name the release
 * being installed and this console's payload, and match the downloaded payload
 * byte for byte. */
static int verify(const http_resp *man, const http_resp *sig, const unsigned char *elf, size_t elf_len,
                  const char *want, char *err, size_t en) {
    int trusted = 0;
    if (sig->len != 64) {
        snprintf(err, en, "the release signature is missing or malformed");
        return -1;
    }
    for (size_t i = 0; i < sizeof UPDATE_KEYS / sizeof UPDATE_KEYS[0] && !trusted; i++)
        trusted = crypto_ed25519_check((const uint8_t *)sig->body, UPDATE_KEYS[i], (const uint8_t *)man->body, man->len) == 0;
    if (!trusted) {
        snprintf(err, en, "the release signature doesn't match; not installing it");
        return -1;
    }
    cJSON *m = cJSON_ParseWithLength(man->body, man->len);
    int rc = -1;
    uint8_t hash[64];
    char hex[129];
    crypto_sha512(hash, elf, elf_len);
    for (int i = 0; i < 64; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    if (!m) snprintf(err, en, "the release manifest is not valid JSON");
    else if (strcmp(jget_str(m, "file", ""), plat_info()->payload) != 0) snprintf(err, en, "the manifest is for %s, not %s", jget_str(m, "file", "?"), plat_info()->payload);
    else if (strcmp(jget_str(m, "version", ""), want) != 0) snprintf(err, en, "the manifest is for version %s, not %s", jget_str(m, "version", "?"), want);
    else if (!newer_than(want, APP_VERSION)) snprintf(err, en, "%s is not newer than this version", want);
    else if (jget_num(m, "size", -1) != (double)elf_len) snprintf(err, en, "the download is incomplete");
    else if (strcmp(jget_str(m, "sha512", ""), hex) != 0) snprintf(err, en, "the download doesn't match the signed checksum");
    else if (str_ends_with_ci(plat_info()->payload, ".elf") && (elf_len < 4 || memcmp(elf, "\x7f" "ELF", 4) != 0))
        snprintf(err, en, "the download is not an ELF payload");
    else rc = 0;
    cJSON_Delete(m);
    return rc;
}

static int progress_cb(void *ud, int64_t done, int64_t total) {
    (void)ud;
    pthread_mutex_lock(&g_lock);
    g_done = done;
    g_total = total;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static void *install_thread(void *arg) {
    (void)arg;
    char want[32], elf_url[512], man_url[512], sig_url[512], path[PATH_MAX_LEN], err[256] = "";
    pthread_mutex_lock(&g_lock);
    str_copy(want, sizeof want, g_latest);
    str_copy(elf_url, sizeof elf_url, g_elf_url);
    str_copy(man_url, sizeof man_url, g_manifest_url);
    str_copy(sig_url, sizeof sig_url, g_sig_url);
    pthread_mutex_unlock(&g_lock);

    path_join(path, sizeof path, plat_data_dir(), "tmp/update.elf");
    http_resp man = {0}, sig = {0}, dl = {0};
    unsigned char *elf = NULL;
    size_t elf_len = 0;
    LOGI("update: downloading %s", want);
    if (http_get_verified(man_url, &man) != 0 || man.status != 200)
        snprintf(err, sizeof err, "can't download the release manifest: %s", man.error);
    else if (http_get_verified(sig_url, &sig) != 0 || sig.status != 200)
        snprintf(err, sizeof err, "can't download the release signature: %s", sig.error);
    else if (http_download_verified(elf_url, path, progress_cb, NULL, &dl) != 0)
        snprintf(err, sizeof err, "can't download %s: %s", plat_info()->payload, dl.error);
    else if (!(elf = (unsigned char *)read_file(path, &elf_len)))
        snprintf(err, sizeof err, "can't read the download");
    else if (verify(&man, &sig, elf, elf_len, want, err, sizeof err) == 0)
        LOGI("update: %s verified (%zu bytes)", want, elf_len);
    http_resp_free(&man);
    http_resp_free(&sig);
    http_resp_free(&dl);
    unlink(path);
    if (err[0]) goto fail;

    /* Restarting mid-sync or mid-game could miss a save, so wait for a quiet moment. */
    for (;;) {
        const char *why = sync_game_running() ? "Waiting for the game to close"
                        : sync_is_running()   ? "Waiting for the sync to finish"
                        : library_busy()      ? "Waiting for downloads to finish"
                        : NULL;
        if (!why) break;
        set_message(U_WAITING, why);
        sleep(2);
    }

    set_message(U_INSTALLING, "Installing");
    int persisted = autostart_replace(elf, elf_len, err, sizeof err);
    if (persisted < 0) goto fail;

    if (!plat_info()->can_relaunch) {
        if (!persisted) {
            snprintf(err, sizeof err, "%s isn't in %s's payload folder, so there's nowhere to install the update. Set up autostart first.",
                     plat_info()->payload, plat_info()->loader);
            goto fail;
        }
        free(elf);
        LOGI("update: %s installed, starts with the next jailbreak", want);
        plat_notify("RomM Sync %s is installed and starts with the next jailbreak", want);
        pthread_mutex_lock(&g_lock);
        g_state = U_PENDING;
        str_copy(g_pending, sizeof g_pending, want);
        str_copy(g_message, sizeof g_message, "Installed. It starts with the next jailbreak.");
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    set_message(U_RESTARTING, persisted ? "Restarting" : "Restarting (until the next reboot, autostart isn't set up)");
    plat_notify("Updating RomM Sync to %s", want);
    sleep(1); /* lets the UI see the restart coming */
    if (plat_launch_elf(elf, elf_len) != 0) {
        snprintf(err, sizeof err, "couldn't start the new version (%s).%s", strerror(errno),
                 persisted ? " It starts with the next reboot." : "");
        goto fail;
    }
    free(elf);
    LOGI("update: started %s, waiting to be replaced", want);
    sleep(RESTART_TIMEOUT_SEC);
    pthread_mutex_lock(&g_lock);
    set_error("the new version didn't start. %s", persisted ? "It starts with the next reboot." : "Send the payload to the console again.");
    pthread_mutex_unlock(&g_lock);
    return NULL;

fail:
    free(elf);
    pthread_mutex_lock(&g_lock);
    set_error("%s", err);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int update_install(char *err, int en) {
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (busy(g_state)) snprintf(err, (size_t)en, "an update check or install is already running");
    else if (!g_latest[0] || !newer_than(g_latest, APP_VERSION)) snprintf(err, (size_t)en, "no newer version found, check again first");
    else if (!strcmp(g_pending, g_latest)) snprintf(err, (size_t)en, "%s is already installed and starts with the next jailbreak", g_latest);
    else if (!g_elf_url[0] || !g_manifest_url[0] || !g_sig_url[0])
        snprintf(err, (size_t)en, "release %s isn't signed for in-app updates, download it from GitHub instead", g_latest);
    else rc = 0;
    if (rc == 0) {
        g_state = U_DOWNLOADING;
        g_error[0] = 0;
        str_copy(g_message, sizeof g_message, "Downloading");
        g_done = g_total = 0;
    }
    pthread_mutex_unlock(&g_lock);
    if (rc == 0 && thread_start(install_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        set_error("cannot start the update");
        pthread_mutex_unlock(&g_lock);
        snprintf(err, (size_t)en, "cannot start the update");
        return -1;
    }
    return rc;
}

cJSON *update_status(void) {
    cJSON *j = cJSON_CreateObject();
    cJSON *as = autostart_status();
    cJSON_AddStringToObject(j, "current", APP_VERSION);
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(j, "state", STATE_NAMES[g_state]);
    cJSON_AddStringToObject(j, "message", g_message);
    cJSON_AddStringToObject(j, "error", g_error);
    cJSON_AddStringToObject(j, "latest", g_latest);
    cJSON_AddBoolToObject(j, "available", g_latest[0] && newer_than(g_latest, APP_VERSION) && strcmp(g_pending, g_latest) != 0);
    cJSON_AddStringToObject(j, "pending", g_pending);
    cJSON_AddBoolToObject(j, "signed", g_manifest_url[0] && g_sig_url[0] && g_elf_url[0]);
    cJSON_AddStringToObject(j, "notes", g_notes ? g_notes : "");
    cJSON_AddStringToObject(j, "published_at", g_published);
    cJSON_AddStringToObject(j, "release_url", g_page_url);
    cJSON_AddNumberToObject(j, "checked_at", (double)g_checked_at);
    cJSON_AddNumberToObject(j, "done", (double)g_done);
    cJSON_AddNumberToObject(j, "total", (double)g_total);
    pthread_mutex_unlock(&g_lock);
    cJSON_AddBoolToObject(j, "autostart", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(as, "installed")));
    cJSON_AddBoolToObject(j, "autostart_supported", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(as, "supported")));
    cJSON_Delete(as);
    return j;
}
