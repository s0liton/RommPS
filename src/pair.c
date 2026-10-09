#include "pair.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "platform.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "util.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_status[16] = "idle"; /* idle | pending | ok | error */
static char g_message[256];
static char g_server[512];
static romm_device_flow g_flow;
static int g_generation; /* stops older poll threads */

static void set_status(const char *st, const char *msg) {
    pthread_mutex_lock(&g_lock);
    str_copy(g_status, sizeof g_status, st);
    str_copy(g_message, sizeof g_message, msg);
    pthread_mutex_unlock(&g_lock);
}

static void normalize_url(const char *in, char *out, size_t n) {
    while (*in == ' ') in++;
    if (strncmp(in, "http://", 7) && strncmp(in, "https://", 8))
        snprintf(out, n, "https://%s", in);
    else
        str_copy(out, n, in);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '/' || out[l - 1] == ' ')) out[--l] = 0;
}

/* -2 if the server's certificate isn't trusted, so the caller doesn't fall back to http. */
static int check_server(const char *base, char *err, int en) {
    char version[32] = "";
    if (romm_heartbeat(base, version, sizeof version) != 0) {
        config_lock();
        int verify = g_cfg.tls_verify;
        config_unlock();
        if (verify && !strncmp(base, "https://", 8) && http_tls_untrusted(base)) {
            snprintf(err, (size_t)en, "%s has a certificate this console doesn't trust (self-signed or from a private CA). "
                     "Tick \"Skip certificate checks\" to connect anyway.", base);
            return -2;
        }
        snprintf(err, (size_t)en, "no RomM server answered at %s", base);
        return -1;
    }
    int major = 0, minor = 0;
    sscanf(version, "%d.%d", &major, &minor);
    if (major < 5 || (major == 5 && minor < 2)) {
        snprintf(err, (size_t)en, "RomM %s found; RomM Sync needs 5.2.0 or newer", version);
        return -1;
    }
    LOGI("RomM %s at %s", version, base);
    config_lock();
    str_copy(g_cfg.server_version, sizeof g_cfg.server_version, version);
    config_unlock();
    return 0;
}

static void finish_pairing(const char *base, const char *token, const char *device_id) {
    config_lock();
    str_copy(g_cfg.server_url, sizeof g_cfg.server_url, base);
    str_copy(g_cfg.token, sizeof g_cfg.token, token);
    str_copy(g_cfg.device_id, sizeof g_cfg.device_id, device_id);
    config_unlock();
    long st = 0;
    cJSON *me = romm_call("GET", "/api/users/me", NULL, &st);
    config_lock();
    if (st == 200) str_copy(g_cfg.username, sizeof g_cfg.username, jget_str(me, "username", ""));
    config_save();
    config_unlock();
    cJSON_Delete(me);
    char msg[160];
    snprintf(msg, sizeof msg, "Paired as %s", g_cfg.username[0] ? g_cfg.username : "(unknown user)");
    set_status("ok", msg);
    LOGI("paired with %s, device %s", base, device_id);
    if (!plat_app_installed()) plat_notify("RomM Sync paired with %s", base);
    sync_new_pairing();
    sync_request("paired");
}

static void *poll_thread(void *arg) {
    int gen = (int)(long)arg;
    char base[512], code[128];
    pthread_mutex_lock(&g_lock);
    str_copy(base, sizeof base, g_server);
    str_copy(code, sizeof code, g_flow.device_code);
    int interval = g_flow.interval, left = g_flow.expires_in;
    pthread_mutex_unlock(&g_lock);

    while (left > 0) {
        sleep((unsigned)interval);
        left -= interval;
        if (gen != g_generation) return NULL;
        char token[256] = "", device_id[64] = "";
        romm_poll r = romm_device_poll(base, code, token, sizeof token, device_id, sizeof device_id);
        if (gen != g_generation) return NULL;
        switch (r) {
        case POLL_OK:
            finish_pairing(base, token, device_id);
            return NULL;
        case POLL_PENDING: break;
        case POLL_SLOW_DOWN: interval += 5; break;
        case POLL_DENIED: set_status("error", "Pairing was denied in RomM"); return NULL;
        case POLL_EXPIRED: set_status("error", "Pairing code expired, start again"); return NULL;
        case POLL_ERROR: break; /* transient, keep trying */
        }
    }
    set_status("error", "Pairing code expired, start again");
    return NULL;
}

/* Checks the server. An address without a scheme is tried as https, then http. */
static int resolve_server(const char *server_url, char *base, size_t n, char *err, int en) {
    normalize_url(server_url, base, n);
    int rc = check_server(base, err, en);
    if (rc == 0) return 0;
    if (rc == -2) return -1;
    const char *s = server_url;
    while (*s == ' ') s++;
    if (!strncmp(s, "http://", 7) || !strncmp(s, "https://", 8)) return -1;
    char alt[512];
    snprintf(alt, sizeof alt, "http://%s", base + 8);
    if (check_server(alt, err, en) != 0) return -1;
    str_copy(base, n, alt);
    return 0;
}

int pair_start(const char *server_url, char *err, int en) {
    char base[512];
    if (resolve_server(server_url, base, sizeof base, err, en) != 0) return -1;
    romm_device_flow flow;
    if (romm_device_init(base, &flow) != 0) {
        snprintf(err, (size_t)en, "server refused the pairing request");
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_flow = flow;
    str_copy(g_server, sizeof g_server, base);
    str_copy(g_status, sizeof g_status, "pending");
    snprintf(g_message, sizeof g_message, "Approve code %s in RomM", flow.user_code);
    int gen = ++g_generation;
    pthread_mutex_unlock(&g_lock);
    thread_start(poll_thread, (void *)(long)gen);
    return 0;
}

int pair_with_code(const char *server_url, const char *code, char *err, int en) {
    char base[512], token[256] = "", device_id[64] = "";
    if (resolve_server(server_url, base, sizeof base, err, en) != 0) return -1;
    if (romm_pair_exchange(base, code, token, sizeof token) != 0) {
        snprintf(err, (size_t)en, "pairing code rejected (they expire after a minute)");
        return -1;
    }
    if (romm_register_device(base, token, device_id, sizeof device_id) != 0) {
        snprintf(err, (size_t)en, "token accepted but device registration failed (needs devices.write scope)");
        return -1;
    }
    g_generation++;
    finish_pairing(base, token, device_id);
    return 0;
}

void pair_forget(void) {
    g_generation++;
    config_lock();
    g_cfg.token[0] = 0;
    g_cfg.device_id[0] = 0;
    g_cfg.username[0] = 0;
    config_save();
    config_unlock();
    /* Sync records belong to the old device. Game matches are kept. */
    state_lock();
    cJSON *s = state_section("saves");
    while (s->child) cJSON_DeleteItemFromObjectCaseSensitive(s, s->child->string);
    cJSON *c = state_section("conflicts");
    while (c->child) cJSON_DeleteItemFromObjectCaseSensitive(c, c->child->string);
    state_save();
    state_unlock();
    set_status("idle", "");
}

cJSON *pair_status(void) {
    cJSON *j = cJSON_CreateObject();
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(j, "status", g_status);
    cJSON_AddStringToObject(j, "message", g_message);
    if (!strcmp(g_status, "pending")) {
        cJSON_AddStringToObject(j, "user_code", g_flow.user_code);
        cJSON_AddStringToObject(j, "verification_url", g_flow.verification_url);
    }
    pthread_mutex_unlock(&g_lock);
    return j;
}
