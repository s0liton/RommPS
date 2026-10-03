#include "web.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "autostart.h"
#include "config.h"
#include "detect.h"
#include "http.h"
#include "library.h"
#include "memcard.h"
#include "pair.h"
#include "platform.h"
#include "profiles.h"
#include "state.h"
#include "sync.h"
#include "ui_index.h" /* generated from ui/index.html */
#include "update.h"
#include "util.h"

#define MAX_HEADER (16 * 1024)
#define MAX_BODY   (2 * 1024 * 1024)
#define MAX_UPLOAD (48 * 1024 * 1024) /* romm-sync.elf for autostart */

static char g_foreground[32];
static pthread_mutex_t g_fg_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_started;

void web_set_foreground(const char *title_id) {
    pthread_mutex_lock(&g_fg_lock);
    str_copy(g_foreground, sizeof g_foreground, title_id ? title_id : "");
    pthread_mutex_unlock(&g_fg_lock);
}

typedef struct {
    int fd;
    char method[8];
    char path[1024];
    char query[2048];
    char *body;
    size_t body_len;
} request;


static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, int status, const char *ctype, const void *body, size_t len,
                          const char *extra_headers) {
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found"
                       : status == 409 ? "Conflict" : status == 502 ? "Bad Gateway" : "Error";
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                     status, reason, ctype, len, extra_headers ? extra_headers : "");
    if (send_all(fd, hdr, (size_t)n) == 0 && len) send_all(fd, body, len);
}

static void send_json(int fd, int status, cJSON *j) {
    char *txt = cJSON_PrintUnformatted(j);
    send_response(fd, status, "application/json", txt ? txt : "null", txt ? strlen(txt) : 4, NULL);
    free(txt);
    cJSON_Delete(j);
}

static void send_error(int fd, int status, const char *msg) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    send_json(fd, status, j);
}

static void send_ok(int fd) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", 1);
    send_json(fd, 200, j);
}


static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') *o++ = ' ';
        else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *o++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else *o++ = *p;
    }
    *o = 0;
}

static int qparam(const request *r, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = r->query; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *end = strchr(v, '&');
            size_t l = end ? (size_t)(end - v) : strlen(v);
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            url_decode(out);
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = 0;
    return 0;
}

static int qint(const request *r, const char *key, int def) {
    char b[32];
    return qparam(r, key, b, sizeof b) && b[0] ? atoi(b) : def;
}

static int read_request(int fd, request *r) {
    char *buf = malloc(MAX_HEADER + 1);
    if (!buf) return -1;
    size_t len = 0;
    char *hdr_end = NULL;
    while (!hdr_end && len < MAX_HEADER) {
        ssize_t n = recv(fd, buf + len, MAX_HEADER - len, 0);
        if (n <= 0) {
            free(buf);
            return -1;
        }
        len += (size_t)n;
        buf[len] = 0;
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    if (!hdr_end) {
        free(buf);
        return -1;
    }
    char target[3072] = "";
    if (sscanf(buf, "%7s %3071s", r->method, target) != 2) {
        free(buf);
        return -1;
    }
    char *q = strchr(target, '?');
    if (q) {
        *q = 0;
        str_copy(r->query, sizeof r->query, q + 1);
    }
    str_copy(r->path, sizeof r->path, target);
    url_decode(r->path);

    size_t clen = 0;
    for (char *h = strstr(buf, "\r\n"); h && h < hdr_end; h = strstr(h + 2, "\r\n")) {
        if (!strncasecmp(h + 2, "Content-Length:", 15)) clen = (size_t)strtoul(h + 17, NULL, 10);
    }
    if (clen > (strcmp(r->path, "/api/autostart/upload") == 0 ? MAX_UPLOAD : MAX_BODY)) {
        free(buf);
        return -1;
    }
    size_t have = len - (size_t)(hdr_end + 4 - buf);
    r->body = malloc(clen + 1);
    if (!r->body) {
        free(buf);
        return -1;
    }
    memcpy(r->body, hdr_end + 4, have > clen ? clen : have);
    while (have < clen) {
        ssize_t n = recv(fd, r->body + have, clen - have, 0);
        if (n <= 0) break;
        have += (size_t)n;
    }
    r->body_len = have < clen ? have : clen;
    r->body[r->body_len] = 0;
    free(buf);
    return 0;
}


static cJSON *status_json(void) {
    cJSON *j = cJSON_CreateObject();
    char ip[64] = "";
    plat_local_ip(ip, sizeof ip);
    cJSON_AddStringToObject(j, "version", APP_VERSION);
    cJSON_AddStringToObject(j, "platform", plat_name());
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddNumberToObject(j, "uptime", (double)(time(NULL) - g_started));
    config_lock();
    cJSON_AddBoolToObject(j, "paired", config_is_paired());
    cJSON_AddBoolToObject(j, "setup_complete", g_cfg.setup_complete);
    cJSON_AddStringToObject(j, "server_url", g_cfg.server_url);
    cJSON_AddStringToObject(j, "username", g_cfg.username);
    cJSON_AddStringToObject(j, "server_version", g_cfg.server_version);
    cJSON_AddStringToObject(j, "device_id", g_cfg.device_id);
    cJSON_AddNumberToObject(j, "web_port", g_cfg.web_port);
    config_unlock();
    pthread_mutex_lock(&g_fg_lock);
    cJSON_AddStringToObject(j, "foreground", g_foreground);
    pthread_mutex_unlock(&g_fg_lock);
    cJSON_AddItemToObject(j, "sync", sync_status_json());
    cJSON_AddItemToObject(j, "pairing", pair_status());
    return j;
}

static const char *image_type(const char *p) {
    char path[1024];
    str_copy(path, sizeof path, p);
    char *q = strchr(path, '?');
    if (q) *q = 0;
    if (str_ends_with_ci(path, ".png")) return "image/png";
    if (str_ends_with_ci(path, ".webp")) return "image/webp";
    if (str_ends_with_ci(path, ".svg")) return "image/svg+xml";
    if (str_ends_with_ci(path, ".ico")) return "image/x-icon";
    return "image/jpeg";
}

/* RomM's platform icons only carry a viewBox, and the PS5 browser renders an
 * unsized SVG at 0x0. Because of course it does. Adds width and height from
 * the viewBox. NULL if nothing changed. */
static char *svg_with_size(const char *svg, size_t len, size_t *out_len) {
    const char *tag = strstr(svg, "<svg");
    if (!tag) return NULL;
    const char *end = strchr(tag, '>');
    if (!end) return NULL;
    /* Already sized? */
    for (const char *q = tag; q < end; q++)
        if (!strncmp(q, " width=", 7)) return NULL;
    const char *vb = strstr(tag, "viewBox=\"");
    double x, y, w = 0, h = 0;
    if (!vb || vb > end || sscanf(vb + 9, "%lf %lf %lf %lf", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0) return NULL;
    char attrs[96];
    int al = snprintf(attrs, sizeof attrs, " width=\"%g\" height=\"%g\"", w, h);
    size_t pos = (size_t)(tag - svg) + 4; /* after "<svg" */
    char *outp = malloc(len + (size_t)al + 1);
    if (!outp) return NULL;
    memcpy(outp, svg, pos);
    memcpy(outp + pos, attrs, (size_t)al);
    memcpy(outp + pos + (size_t)al, svg + pos, len - pos);
    *out_len = len + (size_t)al;
    outp[*out_len] = 0;
    return outp;
}

/* Proxies covers and icons from RomM. Cover paths come with literal spaces in
 * them ("?ts=2026-08-03 07:20:30") and libcurl 8.18 flat out refuses those.
 * Fair enough, so they get encoded here. */
static void proxy_asset(const request *r) {
    char p[1024], base[512], auth[300], url[3200];
    qparam(r, "p", p, sizeof p);
    if (strncmp(p, "/assets/", 8) != 0 || strstr(p, "..")) {
        send_error(r->fd, 400, "bad asset path");
        return;
    }
    config_lock();
    str_copy(base, sizeof base, g_cfg.server_url);
    snprintf(auth, sizeof auth, "Bearer %s", g_cfg.token);
    config_unlock();
    size_t o = (size_t)snprintf(url, sizeof url, "%s", base);
    for (const unsigned char *c = (const unsigned char *)p; *c && o + 4 < sizeof url; c++) {
        if (*c <= 0x20 || *c >= 0x7f || *c == '"' || *c == '<' || *c == '>' || *c == '\\' || *c == '^' ||
            *c == '`' || *c == '{' || *c == '|' || *c == '}')
            o += (size_t)snprintf(url + o, sizeof url - o, "%%%02X", *c);
        else
            url[o++] = (char)*c;
    }
    url[o] = 0;
    http_resp resp;
    if (http_request("GET", url, auth, NULL, NULL, 0, &resp) != 0 || resp.status != 200) {
        http_resp_free(&resp);
        send_error(r->fd, 404, "asset unavailable");
        return;
    }
    const char *type = image_type(p);
    char *body = resp.body;
    size_t len = resp.len;
    char *fixed = !strcmp(type, "image/svg+xml") ? svg_with_size(body, len, &len) : NULL;
    if (fixed) body = fixed;
    send_response(r->fd, 200, type, body, len, "Cache-Control: max-age=86400\r\n");
    free(fixed);
    http_resp_free(&resp);
}

static cJSON *body_json(const request *r) { return r->body_len ? cJSON_Parse(r->body) : cJSON_CreateObject(); }

static void route(request *r) {
    int fd = r->fd;
    int is_get = !strcmp(r->method, "GET"), is_post = !strcmp(r->method, "POST");
    char err[256] = "";

    if (is_get && (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html"))) {
        send_response(fd, 200, "text/html; charset=utf-8", UI_INDEX_HTML, sizeof UI_INDEX_HTML - 1, NULL);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/status")) {
        send_json(fd, 200, status_json());
        return;
    }
    if (is_get && !strcmp(r->path, "/api/log")) {
        char *log = log_recent();
        send_response(fd, 200, "text/plain; charset=utf-8", log ? log : "", log ? strlen(log) : 0, NULL);
        free(log);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/config")) {
        config_lock();
        cJSON *j = config_to_json(0);
        config_unlock();
        send_json(fd, 200, j);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/config")) {
        cJSON *j = body_json(r);
        if (!cJSON_IsObject(j)) {
            cJSON_Delete(j);
            send_error(fd, 400, "invalid JSON");
            return;
        }
        config_lock();
        config_apply_json(j);
        http_set_tls_verify(g_cfg.tls_verify);
        int rc = config_save();
        config_unlock();
        cJSON_Delete(j);
        library_wake();
        if (rc) send_error(fd, 500, "could not save config");
        else send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/pair/start")) {
        cJSON *j = body_json(r);
        int rc = pair_start(jget_str(j, "server_url", ""), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_json(fd, 200, pair_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/pair/code")) {
        cJSON *j = body_json(r);
        int rc = pair_with_code(jget_str(j, "server_url", ""), jget_str(j, "code", ""), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_json(fd, 200, pair_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/pair/forget")) {
        pair_forget();
        send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/update")) {
        send_json(fd, 200, update_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/update/check")) {
        if (update_check(0) != 0) send_error(fd, 409, "an update check or install is already running");
        else send_json(fd, 200, update_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/update/install")) {
        if (update_install(err, sizeof err) != 0) send_error(fd, 409, err);
        else send_json(fd, 200, update_status());
        return;
    }
    if (!config_is_paired() && !strncmp(r->path, "/api/", 5)) {
        send_error(fd, 409, "not paired with a RomM server yet");
        return;
    }
    if (is_get && !strcmp(r->path, "/api/setup/detect")) {
        send_json(fd, 200, detect_emulators());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/setup/emulators")) {
        /* Profiles for the emulators picked in the wizard. */
        cJSON *j = body_json(r), *found = detect_emulators(), *profiles = cJSON_CreateArray();
        const cJSON *root, *c;
        cJSON_ArrayForEach(root, cJSON_GetObjectItemCaseSensitive(j, "roots")) {
            cJSON_ArrayForEach(c, found) {
                if (cJSON_IsString(root) && !strcmp(jget_str(c, "root", ""), root->valuestring))
                    cJSON_AddItemToArray(profiles, cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(c, "profile"), 1));
            }
        }
        cJSON_Delete(found);
        cJSON_Delete(j);
        cJSON *cfg = cJSON_CreateObject();
        if (cJSON_GetArraySize(profiles) > 0) {
            cJSON_AddItemToObject(cfg, "profiles", profiles);
        } else {
            cJSON_Delete(profiles);
            cJSON_AddBoolToObject(cfg, "reset_profiles", 1);
        }
        config_lock();
        config_apply_json(cfg);
        int rc = config_save();
        config_unlock();
        cJSON_Delete(cfg);
        if (rc) send_error(fd, 500, "could not save config");
        else send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/setup/preview")) {
        cJSON *j = sync_preview(err, sizeof err);
        if (j) send_json(fd, 200, j);
        else send_error(fd, 502, err);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/setup/finish")) {
        cJSON *j = body_json(r);
        config_lock();
        config_apply_json(j);
        g_cfg.setup_complete = 1;
        int rc = config_save();
        config_unlock();
        cJSON_Delete(j);
        if (rc) {
            send_error(fd, 500, "could not save config");
            return;
        }
        sync_request("first sync");
        send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/setup/restart")) {
        config_lock();
        g_cfg.setup_complete = 0;
        config_save();
        config_unlock();
        send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/memcards")) {
        send_json(fd, 200, memcard_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/memcards/restore")) {
        cJSON *j = body_json(r);
        int rc = memcard_restore((int)jget_num(j, "slot", 0), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/autostart")) {
        send_json(fd, 200, autostart_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/autostart")) {
        cJSON *j = body_json(r);
        int rc = autostart_set(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "enable")), NULL, 0, err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_json(fd, 200, autostart_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/autostart/upload")) {
        int rc = autostart_set(1, r->body, r->body_len, err, sizeof err);
        if (rc) send_error(fd, 400, err);
        else send_json(fd, 200, autostart_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/sync")) {
        sync_request("manual");
        send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/conflicts/resolve")) {
        cJSON *j = body_json(r);
        int rc = sync_resolve_conflict(jget_str(j, "path", ""), jget_str(j, "keep", ""), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/history")) {
        cJSON *j = sync_history(qint(r, "rom_id", 0), err, sizeof err);
        if (j) send_json(fd, 200, j);
        else send_error(fd, 502, err);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/restore")) {
        cJSON *j = body_json(r);
        int rc = sync_restore(jget_str(j, "kind", ""), (int)jget_num(j, "id", 0), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/strays/move")) {
        cJSON *j = body_json(r);
        int rc = sync_move_stray(jget_str(j, "path", ""), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/platforms")) {
        cJSON *j = library_platforms(err, sizeof err);
        if (j) send_json(fd, 200, j);
        else send_error(fd, 502, err);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/roms")) {
        char search[256];
        qparam(r, "search", search, sizeof search);
        int limit = qint(r, "limit", 60);
        if (limit < 1 || limit > 500) limit = 60;
        cJSON *j = library_roms(qint(r, "platform_id", 0), search, qint(r, "offset", 0), limit, err, sizeof err);
        if (j) send_json(fd, 200, j);
        else send_error(fd, 502, err);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/download")) {
        cJSON *j = body_json(r);
        int rc;
        if (cJSON_GetObjectItemCaseSensitive(j, "rom_id"))
            rc = library_queue_rom((int)jget_num(j, "rom_id", 0), err, sizeof err);
        else
            rc = library_queue_firmware((int)jget_num(j, "platform_id", 0), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/delete")) {
        cJSON *j = body_json(r);
        int rc = library_delete_rom((int)jget_num(j, "rom_id", 0), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/paths")) {
        profile_map *maps = NULL;
        config_lock();
        int n = profiles_expand(g_cfg.profiles, &maps);
        config_unlock();
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "profile", maps[i].profile_name);
            cJSON_AddStringToObject(o, "platforms", maps[i].romm_slugs);
            cJSON_AddStringToObject(o, "emulator", maps[i].emulator);
            cJSON_AddStringToObject(o, "rom_dir", maps[i].rom_dir);
            cJSON_AddStringToObject(o, "save_dir", maps[i].sync_saves ? maps[i].save_dir : "");
            cJSON_AddStringToObject(o, "state_dir", maps[i].sync_saves ? maps[i].state_dir : "");
            cJSON_AddStringToObject(o, "bios_dir", maps[i].bios_dir);
            cJSON_AddStringToObject(o, "sync_note", maps[i].sync_note);
            cJSON_AddStringToObject(o, "save_layout", maps[i].save_layout);
            cJSON_AddItemToArray(arr, o);
        }
        free(maps);
        send_json(fd, 200, arr);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/downloads")) {
        send_json(fd, 200, library_downloads());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/downloads/cancel")) {
        cJSON *j = body_json(r);
        int rc = library_cancel((int)jget_num(j, "id", 0), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/downloads/clear")) {
        library_clear_finished();
        send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/rescan")) {
        /* Try unmatched games again on the next sync. */
        state_lock();
        cJSON *roms = state_section("roms"), *e = roms->child;
        while (e) {
            cJSON *next = e->next;
            if ((int)jget_num(e, "rom_id", 0) == 0) cJSON_DeleteItemFromObjectCaseSensitive(roms, e->string);
            e = next;
        }
        state_save();
        state_unlock();
        sync_request("rescan");
        send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/tile")) {
        int port;
        config_lock();
        port = g_cfg.web_port;
        config_unlock();
        if (plat_install_tile(port) == 0) send_ok(fd);
        else send_error(fd, 500, "tile install failed or unsupported on this platform");
        return;
    }
    if (is_get && !strcmp(r->path, "/cover")) {
        proxy_asset(r);
        return;
    }
    send_error(fd, 404, "not found");
}

static void *conn_thread(void *arg) {
    int fd = (int)(long)arg;
    struct timeval tv = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    request r;
    memset(&r, 0, sizeof r);
    r.fd = fd;
    if (read_request(fd, &r) == 0) route(&r);
    free(r.body);
    close(fd);
    return NULL;
}

static int open_listener(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return -1;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) != 0 || listen(srv, 16) != 0) {
        int err = errno;
        close(srv);
        errno = err;
        return -1;
    }
    return srv;
}

static int g_port, g_srv = -1, g_reopened;

int web_take_reopened(void) {
    return __atomic_exchange_n(&g_reopened, 0, __ATOMIC_SEQ_CST);
}

/* Issue #1 - Rest mode tears down the console's sockets, and the listener never accepts
 * again after wake-up. When accept keeps failing, open a fresh listener (ftpsrv
 * does the same). A single failure (an aborted connection, running out of fds)
 * is not enough, or we would announce a resume that never happened. */
#define ACCEPT_FAILS_BEFORE_REOPEN 3

static void *accept_thread(void *arg) {
    (void)arg;
    int fails = 0;
    for (;;) {
        if (g_srv < 0) {
            g_srv = open_listener(g_port);
            if (g_srv < 0) {
                sleep(3);
                continue;
            }
            LOGI("web UI listening again on port %d", g_port);
            __atomic_store_n(&g_reopened, 1, __ATOMIC_SEQ_CST);
        }
        int fd = accept(g_srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (++fails < ACCEPT_FAILS_BEFORE_REOPEN) {
                LOGW("accept failed: %s", strerror(errno));
                usleep(250 * 1000);
                continue;
            }
            LOGW("accept failed (%s), reopening the listener", strerror(errno));
            close(g_srv);
            g_srv = -1;
            fails = 0;
            continue;
        }
        fails = 0;
        if (thread_start(conn_thread, (void *)(long)fd) != 0) close(fd);
    }
    return NULL;
}

int web_start(int port) {
    g_started = time(NULL);
    g_port = port;
    g_srv = open_listener(port);
    if (g_srv < 0) LOGE("cannot listen on port %d: %s; retrying", port, strerror(errno));
    else LOGI("web UI listening on port %d", port);
    thread_start(accept_thread, NULL);
    return g_srv < 0 ? -1 : 0;
}
