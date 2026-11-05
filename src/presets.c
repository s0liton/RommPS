#include "presets.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#include "platform.h"
#include "util.h"
#include "config.h"
#include "detect.h"
#include "http.h"
#include "monocypher-ed25519.h"
#include "update_keys.h"
#include "state.h"

#ifndef PRESETS_URL
#define PRESETS_URL "https://raw.githubusercontent.com/s0liton/RommPS/presets"
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static cJSON *g_file;

static char g_source[16] = "built-in";

static int g_checking;
static char g_error[256];
static time_t g_checked_at;



int presets_version_cmp(const char *a, const char *b) {
    int x[4] = {0}, y[4] = {0};
    sscanf(a, "%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3]);
    sscanf(b, "%d.%d.%d.%d", &y[0], &y[1], &y[2], &y[3]);
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static void download_path(const char *name, char *out, size_t n) {
    char dir[PATH_MAX_LEN];
    path_join(dir, sizeof dir, plat_data_dir(), "presets");
    path_join(out, n, dir, name);
}

/* One string's placeholders, when catalog_var knows them. */
static void resolve_string(cJSON *s, int (*var)(const char *, char *, size_t)) {
    const char *in = s->valuestring;
    if (!strchr(in, '{')) return;
    char out[PATH_MAX_LEN * 2], name[64], value[PATH_MAX_LEN];
    size_t o = 0;
    int changed = 0;
    while (*in && o < sizeof out - 1) {
        const char *end = *in == '{' ? strchr(in, '}') : NULL;
        if (end && (size_t)(end - in - 1) < sizeof name) {
            snprintf(name, sizeof name, "%.*s", (int)(end - in - 1), in + 1);
            if (var(name, value, sizeof value)) {
                o += (size_t)snprintf(out + o, sizeof out - o, "%s", value);
                if (o >= sizeof out) return; /* too long: keep it as it was */
                in = end + 1;
                changed = 1;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = 0;
    if (changed) cJSON_SetValuestring(s, out);
}

void presets_resolve(cJSON *node) {
    int (*var)(const char *, char *, size_t) = plat_info()->catalog_var;
    if (!var || !node) return;
    cJSON *c;
    cJSON_ArrayForEach(c, node) {
        if (cJSON_IsString(c)) resolve_string(c, var);
        else presets_resolve(c);
    }
}

static cJSON *usable(cJSON *f, const char *from) {
    const char *why = !cJSON_IsObject(f) ? "Not valid JSON"
                    : jget_num(f, "format", 0) != PRESETS_FORMAT ? "In a format this version cannot read"
                    : strcmp(jget_str(f, "console", ""), plat_info()->name) != 0 ? "Not for this console"
                    : presets_version_cmp(jget_str(f, "version", ""), "0") <= 0 ? "Missing its version value"
                    : !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(f, "emulators")) ? "Missing emulators section."
                    : NULL;
    if (!why) {
        presets_resolve(f);
        return f;
    }
    LOGW("Presets: %s is %s; Not using it", from, why);
    cJSON_Delete(f);
    return NULL;
}

static cJSON *read_download(const char *name) {
    char path[PATH_MAX_LEN];
    download_path(name, path, sizeof path);
    char *txt = read_file(path, NULL);
    if (!txt) return NULL; /* A presets file was never downloaded. */
    cJSON *f = cJSON_Parse(txt);
    free(txt);
    return usable(f, name);
}

static void remove_downloads(void) {
    char path[PATH_MAX_LEN];
    download_path("current.json", path, sizeof path);
    unlink(path);
    download_path("previous.json", path, sizeof path);
    unlink(path);
}

void presets_init(void) {
    const char *embedded = plat_info()->emulator_catalog;
    cJSON *best = embedded ? usable (cJSON_Parse(embedded), "The built-in presets file") : NULL;
    cJSON *dl = read_download("current.json");
    if (!dl) dl = read_download("previous.json");
    if (dl && (!best || presets_version_cmp(jget_str(dl, "version", ""), jget_str(best, "version", "")) > 0)) {
        cJSON_Delete(best);
        best = dl;
        str_copy(g_source, sizeof g_source, "download");
    } else if (dl) {
        /* A newer RommPS brought newer presets than the last download */
        LOGI("presets: built-in %s is newer than the download (%s), let's remove it", jget_str(best, "version", ""), jget_str(dl, "version", ""));
        cJSON_Delete(dl);
        remove_downloads();
    }
    pthread_mutex_lock(&g_lock);
    g_file = best;
    pthread_mutex_unlock(&g_lock);
    LOGI("presets: %s (%s)", best ? jget_str(best, "version", "") : "none", g_source);
}

const cJSON *presets_catalog(void) {
    pthread_mutex_lock(&g_lock);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(g_file, "emulators");
    pthread_mutex_unlock(&g_lock);
    return c;
}

/* Where presets-<console-name>.json and .sig file are located. */
static const char *presets_url(void) {
    const char *u = getenv("ROMM_SYNC_PRESETS_URL"); /* Used for tests. */
    return u && *u ? u: PRESETS_URL;
}

static int fetch(const char *url, http_resp *r, char *err, size_t en) {
    if (http_get_verified(url, r) != 0) snprintf( err, en, "Can't reach %s: %s", url, r->error);
    else if (r->status != 200) snprintf(err, en, "%s answered with HTTP %ld", url, r->status);
    else return 0;
    return -1;
}

/*  Check to see if the presets file is signed by our release key */
static int signed_by_us(const http_resp *file, const http_resp *sig) {
    if (sig->len != 64) return 0;
    for (size_t i = 0; i < sizeof UPDATE_KEYS / sizeof UPDATE_KEYS[0]; i++)
        if (crypto_ed25519_check((const uint8_t *)sig->body, UPDATE_KEYS[i], (const uint8_t *)file->body, file->len) == 0)
            return 1;
    return 0;
}

static int save_download(const http_resp *file, char *err, size_t en) {
    char cur[PATH_MAX_LEN], prev[PATH_MAX_LEN];
    download_path("current.json", cur, sizeof cur);
    download_path("previous.json", prev, sizeof prev);
    mkdir_parent(cur);
    if (file_exists(cur) && move_file(cur, prev) !=0) LOGW("presets: can't keep the previous download");
    if (write_file_atomic(cur, file->body, file->len) == 0) return 0;
    snprintf(err, en, "can't save the presets to %s", cur);
    return -1;
}

static void apply(cJSON *f) {
    pthread_mutex_lock(&g_lock);
    g_file = f;
    str_copy(g_source, sizeof g_source, "download");
    pthread_mutex_unlock(&g_lock);
    config_lock();
    int changed = g_cfg.profiles_custom &&  detect_refresh_profiles(g_cfg.profiles) > 0;
    if (changed) config_save();
    config_unlock();
    LOGI("presets: now %s%s", jget_str(f, "version", ""), changed ? ", saved profiles updated" : "");
}

static void *check_thread(void *arg) {
    (void)arg;
    char url[600], sig_url[600], have[32], err[256] = "";
    http_resp file = {0}, sig = {0};
    cJSON *f = NULL;

    snprintf(url, sizeof url, "%s/presets-%s.json", presets_url(), plat_info()->name);
    snprintf(sig_url, sizeof sig_url, "%s/presets-%s.sig", presets_url(), plat_info()->name);

    if(fetch(url, &file, err, sizeof err) == 0  && fetch(sig_url, &sig, err, sizeof err) == 0) {
        if (!signed_by_us(&file, &sig)) snprintf(err, sizeof err, "the presets signature doesn't match, so not using them");
        else if (!(f = usable(cJSON_ParseWithLength(file.body, file.len), "the download")))
            snprintf(err, sizeof err, "the published presets can't be used here");
    }
    if (f) {
        pthread_mutex_lock(&g_lock);
        str_copy(have, sizeof have, g_file ? jget_str(g_file, "version", "0") : "0");
        pthread_mutex_unlock(&g_lock);
        if (presets_version_cmp(jget_str(f, "version", ""), have) <= 0) {
            LOGI("presets: %s is current", have);
            cJSON_Delete(f);
        } else if (save_download(&file, err, sizeof err) == 0) {
            apply(f);
        } else {
            cJSON_Delete(f);
        }

    }
    if (err[0]) LOGW("presets: %s", err);
    http_resp_free(&file);
    http_resp_free(&sig);

    pthread_mutex_lock(&g_lock);
    g_checking = 0;
    g_checked_at = time(NULL);
    str_copy(g_error, sizeof g_error, err);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int presets_check(void) {
    pthread_mutex_lock(&g_lock);
    int busy = g_checking;
    g_checking = 1;
    pthread_mutex_unlock(&g_lock);
    if (busy) return -1;
    if (thread_start(check_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_checking = 0;
        str_copy(g_error, sizeof g_error, "cannot start the presets check");
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    return 0;
}

cJSON *presets_status(void) {
    cJSON *j = cJSON_CreateObject();
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(j, "version", g_file ? jget_str(g_file, "version", "") : "");
    cJSON_AddStringToObject(j, "source", g_source);
    cJSON_AddStringToObject(j, "state", g_checking ? "checking" : g_error[0] ? "error" : "idle");
    cJSON_AddStringToObject(j, "error", g_error);
    cJSON_AddNumberToObject(j, "checked_at", (double)g_checked_at);
    pthread_mutex_unlock(&g_lock);
    return j;
}
