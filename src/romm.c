#include "romm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "platform.h"
#include "state.h"
#include "util.h"

static void current_auth(char *base, size_t bn, char *auth, size_t an) {
    config_lock();
    str_copy(base, bn, g_cfg.server_url);
    if (g_cfg.token[0])
        snprintf(auth, an, "Bearer %s", g_cfg.token);
    else
        auth[0] = 0;
    config_unlock();
}

cJSON *romm_call_raw(const char *base, const char *auth, const char *method, const char *path,
                     const cJSON *body, long *status) {
    char url[2048];
    snprintf(url, sizeof url, "%s%s", base, path);
    char *payload = body ? cJSON_PrintUnformatted(body) : NULL;
    http_resp r;
    int rc = http_request(method, url, auth, payload ? "application/json" : NULL, payload,
                          payload ? strlen(payload) : 0, &r);
    free(payload);
    if (status) *status = rc == 0 ? r.status : 0;
    cJSON *j = (rc == 0 && r.body) ? cJSON_Parse(r.body) : NULL;
    /* A big reply can't always be parsed where memory is tight (the PS4 payload
     * shares GoldHEN's process); romm_list pages lists to stay small. */
    if (rc == 0 && r.status < 300 && r.len && !j)
        LOGW("%s %s -> %ld, but the %zu-byte reply couldn't be read (out of memory?)", method, path, r.status, r.len);
    if (rc == 0 && r.status >= 400) {
        char d[256];
        romm_error_detail(j, d, sizeof d);
        /* Normal until the user approves the pairing. */
        if (strcmp(d, "authorization_pending") != 0) LOGW("%s %s -> %ld %s", method, path, r.status, d);
    }
    http_resp_free(&r);
    return j;
}

int romm_list(const char *path, int offset, int limit, romm_list_cb cb, void *ctx, long *status) {
    int seen = 0;
    if (status) *status = 0;
    while (seen < limit) {
        char url[2048];
        int want = limit - seen < ROMM_LIST_PAGE ? limit - seen : ROMM_LIST_PAGE;
        snprintf(url, sizeof url, "%s%soffset=%d&limit=%d", path, strchr(path, '?') ? "&" : "?", offset + seen, want);
        long st = 0;
        cJSON *page = romm_call("GET", url, NULL, &st);
        if (status) *status = st;
        if (st != 200 || !page) {
            cJSON_Delete(page);
            return -1;
        }
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(page, "items"), *it;
        int got = cJSON_GetArraySize(items), stop = 0;
        cJSON_ArrayForEach(it, items) {
            if ((stop = cb(it, page, seen == 0, ctx)) != 0) break;
        }
        if (got == 0 && seen == 0) cb(NULL, page, 1, ctx); /* an empty list still has a total */
        cJSON_Delete(page);
        seen += got;
        if (stop || got < want) break;
    }
    return seen;
}

cJSON *romm_call(const char *method, const char *path, const cJSON *body, long *status) {
    char base[512], auth[300];
    current_auth(base, sizeof base, auth, sizeof auth);
    if (!base[0]) {
        if (status) *status = 0;
        return NULL;
    }
    return romm_call_raw(base, auth, method, path, body, status);
}

void romm_error_detail(const cJSON *j, char *out, int n) {
    out[0] = 0;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "detail");
    if (cJSON_IsString(d)) {
        str_copy(out, (size_t)n, d->valuestring);
    } else if (d) {
        char *s = cJSON_PrintUnformatted(d);
        str_copy(out, (size_t)n, s);
        free(s);
    }
}

int romm_heartbeat(const char *base, char *version, int vn) {
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "GET", "/api/heartbeat", NULL, &st);
    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(j, "SYSTEM");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(sys, "VERSION");
    int ok = st == 200 && cJSON_IsString(v);
    if (ok && version) str_copy(version, (size_t)vn, v->valuestring);
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

static cJSON *scopes_array(void) {
    static const char *scopes[] = {ROMM_SCOPES};
    return cJSON_CreateStringArray(scopes, (int)(sizeof scopes / sizeof scopes[0]));
}

int romm_device_init(const char *base, romm_device_flow *flow) {
    cJSON *body = cJSON_CreateObject();
    config_lock();
    cJSON_AddStringToObject(body, "client_device_identifier", g_cfg.client_device_identifier);
    cJSON_AddStringToObject(body, "name", g_cfg.device_name);
    config_unlock();
    cJSON_AddStringToObject(body, "client", plat_info()->client);
    cJSON_AddStringToObject(body, "platform", plat_info()->console);
    cJSON_AddStringToObject(body, "client_version", APP_VERSION);
    cJSON_AddItemToObject(body, "requested_scopes", scopes_array());
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "POST", "/api/auth/device/init", body, &st);
    cJSON_Delete(body);
    int rc = -1;
    if ((st == 200 || st == 201) && j) {
        memset(flow, 0, sizeof *flow);
        str_copy(flow->device_code, sizeof flow->device_code, jget_str(j, "device_code", ""));
        str_copy(flow->user_code, sizeof flow->user_code, jget_str(j, "user_code", ""));
        snprintf(flow->verification_url, sizeof flow->verification_url, "%s%s", base,
                 jget_str(j, "verification_path_complete", ""));
        const cJSON *iv = cJSON_GetObjectItemCaseSensitive(j, "interval");
        const cJSON *ex = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
        flow->interval = cJSON_IsNumber(iv) && iv->valueint > 0 ? iv->valueint : 5;
        flow->expires_in = cJSON_IsNumber(ex) ? ex->valueint : 600;
        rc = flow->device_code[0] ? 0 : -1;
    }
    cJSON_Delete(j);
    return rc;
}

romm_poll romm_device_poll(const char *base, const char *device_code, char *token, int tn,
                           char *device_id, int dn) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_code", device_code);
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "POST", "/api/auth/device/token", body, &st);
    cJSON_Delete(body);
    romm_poll res = POLL_ERROR;
    if (st == 200 && j) {
        str_copy(token, (size_t)tn, jget_str(j, "access_token", ""));
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "device_id");
        if (cJSON_IsString(d))
            str_copy(device_id, (size_t)dn, d->valuestring);
        else if (cJSON_IsNumber(d))
            snprintf(device_id, (size_t)dn, "%d", d->valueint);
        res = token[0] ? POLL_OK : POLL_ERROR;
    } else if (st >= 400 && st < 500) {
        char d[128];
        romm_error_detail(j, d, sizeof d);
        if (strstr(d, "authorization_pending")) res = POLL_PENDING;
        else if (strstr(d, "slow_down") || st == 429) res = POLL_SLOW_DOWN;
        else if (strstr(d, "access_denied")) res = POLL_DENIED;
        else if (strstr(d, "expired")) res = POLL_EXPIRED;
    }
    cJSON_Delete(j);
    return res;
}

int romm_pair_exchange(const char *base, const char *code, char *token, int tn) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "code", code);
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "POST", "/api/client-tokens/exchange", body, &st);
    cJSON_Delete(body);
    int rc = -1;
    if (st == 200 && j) {
        str_copy(token, (size_t)tn, jget_str(j, "raw_token", ""));
        rc = token[0] ? 0 : -1;
    }
    cJSON_Delete(j);
    return rc;
}

int romm_register_device(const char *base, const char *token, char *device_id, int dn) {
    char auth[300], ip[64] = "";
    snprintf(auth, sizeof auth, "Bearer %s", token);
    plat_local_ip(ip, sizeof ip);
    cJSON *body = cJSON_CreateObject();
    config_lock();
    cJSON_AddStringToObject(body, "name", g_cfg.device_name);
    config_unlock();
    cJSON_AddStringToObject(body, "platform", plat_info()->console);
    cJSON_AddStringToObject(body, "client", plat_info()->client);
    cJSON_AddStringToObject(body, "client_version", APP_VERSION);
    if (ip[0]) cJSON_AddStringToObject(body, "ip_address", ip);
    cJSON_AddStringToObject(body, "sync_mode", "api");
    cJSON_AddBoolToObject(body, "allow_existing", 1);
    long st = 0;
    cJSON *j = romm_call_raw(base, auth, "POST", "/api/devices", body, &st);
    cJSON_Delete(body);
    int rc = -1;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "device_id");
    if (!d) d = cJSON_GetObjectItemCaseSensitive(j, "id");
    if ((st == 200 || st == 201) && d) {
        if (cJSON_IsString(d))
            str_copy(device_id, (size_t)dn, d->valuestring);
        else
            snprintf(device_id, (size_t)dn, "%d", d->valueint);
        rc = 0;
    }
    cJSON_Delete(j);
    return rc;
}

int romm_download(const char *path, const char *dest, http_progress_fn cb, void *ud,
                  http_resp *resp) {
    char base[512], auth[300], url[2048];
    current_auth(base, sizeof base, auth, sizeof auth);
    snprintf(url, sizeof url, "%s%s", base, path);
    return http_download(url, auth, dest, 1, cb, ud, resp);
}

cJSON *romm_upload(const char *method, const char *path, const char *field, const char *file,
                   const char *upload_name, long *status) {
    char base[512], auth[300], url[2048];
    current_auth(base, sizeof base, auth, sizeof auth);
    snprintf(url, sizeof url, "%s%s", base, path);
    http_resp r;
    int rc = http_upload_file(method, url, auth, field, file, upload_name, &r);
    if (status) *status = rc == 0 ? r.status : 0;
    cJSON *j = (rc == 0 && r.body) ? cJSON_Parse(r.body) : NULL;
    if (rc == 0 && r.status >= 400) {
        char d[256];
        romm_error_detail(j, d, sizeof d);
        LOGW("%s %s -> %ld %s", method, path, r.status, d);
    }
    http_resp_free(&r);
    return j;
}
