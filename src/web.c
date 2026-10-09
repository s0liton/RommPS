#include "web.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "autostart.h"
#include "config.h"
#include "covers.h"
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

/* The UI with the theme already on <html>, so the first frame is right. */
static void send_page(int fd) {
    char attr[64], hdr[256];
    config_lock();
    snprintf(attr, sizeof attr, " data-theme=\"%s\"", g_cfg.ui_theme);
    config_unlock();
    const char *page = UI_INDEX_HTML, *at = strstr(page, "<html");
    size_t len = sizeof UI_INDEX_HTML - 1, head = at ? (size_t)(at - page) + 5 : 0;
    if (!at) attr[0] = 0;
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     len + strlen(attr));
    if (send_all(fd, hdr, (size_t)n) == 0 && send_all(fd, page, head) == 0 && send_all(fd, attr, strlen(attr)) == 0)
        send_all(fd, page + head, len - head);
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
    cJSON_AddStringToObject(j, "console", plat_info()->console);
    cJSON_AddStringToObject(j, "loader", plat_info()->loader);
    cJSON_AddBoolToObject(j, "has_tile", plat_info()->has_tile);
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
    cJSON_AddItemToObject(j, "covers", covers_status());
    return j;
}

/* Systems more than one enabled emulator can play, with the one that does:
 * [{system, options: [{profile, name, emulator}], selected}]. */
static cJSON *systems_json(const cJSON *profiles) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *p, *e;
    cJSON_ArrayForEach(p, profiles) {
        if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "enabled"))) continue;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
            const char *key = profiles_system_key(e);
            if (!key[0]) continue;
            cJSON *sys = NULL, *s;
            cJSON_ArrayForEach(s, out) if (!strcmp(jget_str(s, "system", ""), key)) sys = s;
            if (!sys) {
                sys = cJSON_CreateObject();
                cJSON_AddStringToObject(sys, "system", key);
                cJSON_AddArrayToObject(sys, "options");
                cJSON_AddStringToObject(sys, "selected", "");
                cJSON_AddItemToArray(out, sys);
            }
            const char *id = jget_str(p, "id", "");
            cJSON *opts = cJSON_GetObjectItemCaseSensitive(sys, "options"), *o;
            int dup = 0;
            cJSON_ArrayForEach(o, opts) if (!strcmp(jget_str(o, "profile", ""), id)) dup = 1;
            if (dup) continue;
            o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "profile", id);
            cJSON_AddStringToObject(o, "name", jget_str(p, "name", id));
            cJSON_AddStringToObject(o, "emulator", jget_str(e, "core_name", jget_str(e, "emulator", "")));
            cJSON_AddItemToArray(opts, o);
            if (!jget_str(sys, "selected", "")[0] && !profiles_excludes(p, key))
                jset_str(sys, "selected", id);
        }
    }
    for (int i = cJSON_GetArraySize(out) - 1; i >= 0; i--)
        if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(out, i), "options")) < 2)
            cJSON_DeleteItemFromArray(out, i);
    return out;
}

static int plays_system(const cJSON *profile, const char *key) {
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(profile, "platforms"))
        if (!strcmp(profiles_system_key(e), key)) return 1;
    return 0;
}

/* Hands a system to one emulator: the others that play it exclude it.
 * -1 (and nothing changed) if that emulator doesn't play it. */
static int choose_system(cJSON *profiles, const char *key, const char *id) {
    cJSON *p;
    int found = 0;
    cJSON_ArrayForEach(p, profiles) if (!strcmp(jget_str(p, "id", ""), id) && plays_system(p, key)) found = 1;
    if (!found) return -1;
    cJSON_ArrayForEach(p, profiles) {
        if (!plays_system(p, key)) continue;
        cJSON *ex = cJSON_GetObjectItemCaseSensitive(p, "exclude");
        if (!cJSON_IsArray(ex)) {
            cJSON_DeleteItemFromObjectCaseSensitive(p, "exclude");
            ex = cJSON_AddArrayToObject(p, "exclude");
        }
        for (int i = cJSON_GetArraySize(ex) - 1; i >= 0; i--) {
            const cJSON *x = cJSON_GetArrayItem(ex, i);
            if (cJSON_IsString(x) && str_ieq(x->valuestring, key)) cJSON_DeleteItemFromArray(ex, i);
        }
        if (strcmp(jget_str(p, "id", ""), id) != 0) cJSON_AddItemToArray(ex, cJSON_CreateString(key));
        else if (!cJSON_GetArraySize(ex)) cJSON_DeleteItemFromObjectCaseSensitive(p, "exclude");
    }
    return 0;
}

/* A growing text buffer for the diagnostics report. */
typedef struct { char *s; size_t len, cap; } textbuf;

static void tb_add(textbuf *b, const char *txt, size_t n) {
    if (!txt) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->len + n + 1) * 2;
        char *s = realloc(b->s, cap);
        if (!s) return;
        b->s = s;
        b->cap = cap;
    }
    memcpy(b->s + b->len, txt, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void tb_str(textbuf *b, const char *txt) { if (txt) tb_add(b, txt, strlen(txt)); }

static void tb_json(textbuf *b, const char *title, cJSON *j) {
    char *txt = cJSON_Print(j);
    tb_str(b, "\n== ");
    tb_str(b, title);
    tb_str(b, "\n");
    tb_str(b, txt ? txt : "null");
    tb_str(b, "\n");
    free(txt);
    cJSON_Delete(j);
}

/* The last max bytes of a file, from the start of a line. */
static void tb_file_tail(textbuf *b, const char *title, const char *path, size_t max) {
    size_t len = 0;
    char *txt = read_file(path, &len);
    if (!txt) return;
    const char *start = txt;
    if (len > max) {
        start = txt + len - max;
        const char *nl = strchr(start, '\n');
        if (nl) start = nl + 1;
    }
    tb_str(b, "\n== ");
    tb_str(b, title);
    tb_str(b, "\n");
    tb_str(b, start);
    free(txt);
}

/* Everything a bug report needs, as one text file: what the console is, the
 * settings (no token), the state of sync, autostart and updates, and the log
 * with debug lines. */
static char *diagnostics_text(void) {
    textbuf b = {0};
    char line[512], about[256], now_iso[40], path[PATH_MAX_LEN];
    plat_describe(about, sizeof about);
    iso8601_utc(time(NULL), now_iso, sizeof now_iso);
    snprintf(line, sizeof line, "RomM Sync diagnostics\nversion %s, build %s, %s\n%s\nclock %s, up %lds\n",
             APP_VERSION, APP_BUILD, plat_name(), about, now_iso, (long)(time(NULL) - g_started));
    tb_str(&b, line);
    tb_json(&b, "status", status_json());
    tb_json(&b, "autostart", autostart_status());
    tb_json(&b, "update", update_status());
    config_lock();
    cJSON *cfg = config_to_json(0);
    config_unlock();
    tb_json(&b, "settings", cfg);
    path_join(path, sizeof path, plat_data_dir(), "boot.log");
    tb_file_tail(&b, "boot.log", path, 4096);
    path_join(path, sizeof path, plat_data_dir(), "launcher.log"); /* PS4 home screen app */
    tb_file_tail(&b, "launcher.log", path, 8192);
    path_join(path, sizeof path, plat_data_dir(), "romm-sync.log.1");
    tb_file_tail(&b, "romm-sync.log.1 (previous)", path, 64 * 1024);
    path_join(path, sizeof path, plat_data_dir(), "romm-sync.log");
    tb_file_tail(&b, "romm-sync.log", path, 256 * 1024);
    return b.s;
}

/* Covers and platform icons, from the console's cache or RomM (covers.h). */
static void proxy_asset(const request *r) {
    char p[1024];
    qparam(r, "p", p, sizeof p);
    char *body = NULL;
    size_t len = 0;
    const char *type = "image/jpeg";
    int rc = covers_fetch(p, &body, &len, &type);
    if (rc == 0) send_response(r->fd, 200, type, body, len, "Cache-Control: max-age=86400\r\n");
    else send_error(r->fd, rc == 1 ? 404 : (strncmp(p, "/assets/", 8) ? 400 : 404), rc == 1 ? "asset unavailable" : "bad asset path");
    free(body);
}

static cJSON *body_json(const request *r) { return r->body_len ? cJSON_Parse(r->body) : cJSON_CreateObject(); }

/* Where the folder picker may look: internal storage and the drives the
 * console mounts (ROMM_SYNC_FS_ROOTS, comma separated, replaces them in tests). */
static int fs_roots(char roots[][PATH_MAX_LEN], int max) {
    const char *env = getenv("ROMM_SYNC_FS_ROOTS");
    char list[1024];
    str_copy(list, sizeof list, env && *env ? env : "/data,/mnt/usb0,/mnt/usb1,/mnt/usb2,/mnt/usb3,/mnt/ext0,/mnt/ext1");
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t && n < max; t = strtok_r(NULL, ",", &save))
        if (dir_exists(t)) str_copy(roots[n++], PATH_MAX_LEN, t);
    return n;
}

/* The folders in a folder, for the setup's folder picker: {path, parent,
 * folders}. No path: the roots. Only folders, and only below a root. */
static cJSON *fs_folders(const char *path, char *err, int en) {
    char roots[8][PATH_MAX_LEN];
    int nroots = fs_roots(roots, 8);
    cJSON *j = cJSON_CreateObject(), *dirs = cJSON_CreateArray();
    if (!path[0]) {
        cJSON_AddStringToObject(j, "path", "");
        cJSON_AddStringToObject(j, "parent", "");
        for (int i = 0; i < nroots; i++) cJSON_AddItemToArray(dirs, cJSON_CreateString(roots[i]));
        cJSON_AddItemToObject(j, "folders", dirs);
        return j;
    }
    char clean[PATH_MAX_LEN];
    str_copy(clean, sizeof clean, path);
    size_t l = strlen(clean);
    while (l > 1 && clean[l - 1] == '/') clean[--l] = 0;
    int inside = 0;
    for (int i = 0; i < nroots; i++) {
        size_t rl = strlen(roots[i]);
        if (!strncmp(clean, roots[i], rl) && (clean[rl] == 0 || clean[rl] == '/')) inside = 1;
    }
    if (clean[0] != '/' || strstr(clean, "/..") || strstr(clean, "/./") || !inside || !dir_exists(clean)) {
        cJSON_Delete(j);
        cJSON_Delete(dirs);
        snprintf(err, (size_t)en, "not a folder the console can use");
        return NULL;
    }
    DIR *d = opendir(clean);
    struct dirent *de;
    char (*names)[256] = malloc(512 * sizeof *names); /* on the heap: console threads have small stacks */
    int n = 0;
    while (names && d && (de = readdir(d)) && n < 512) {
        if (de->d_name[0] == '.') continue;
        char full[PATH_MAX_LEN];
        path_join(full, sizeof full, clean, de->d_name);
        if (dir_exists(full)) str_copy(names[n++], sizeof names[0], de->d_name);
    }
    if (d) closedir(d);
    if (names) {
        qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))strcasecmp);
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(dirs, cJSON_CreateString(names[i]));
        free(names);
    }
    /* Up stops at a root. */
    char parent[PATH_MAX_LEN] = "";
    int at_root = 0;
    for (int i = 0; i < nroots; i++) at_root |= !strcmp(clean, roots[i]);
    if (!at_root) {
        str_copy(parent, sizeof parent, clean);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) *slash = 0;
    }
    cJSON_AddStringToObject(j, "path", clean);
    cJSON_AddStringToObject(j, "parent", parent);
    cJSON_AddItemToObject(j, "folders", dirs);
    return j;
}

static void route(request *r) {
    int fd = r->fd;
    int is_get = !strcmp(r->method, "GET"), is_post = !strcmp(r->method, "POST");
    char err[256] = "";

    if (is_get && (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html"))) {
        send_page(fd);
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
    if (is_get && !strcmp(r->path, "/api/systems")) {
        config_lock();
        cJSON *j = systems_json(g_cfg.profiles);
        config_unlock();
        send_json(fd, 200, j);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/systems")) {
        cJSON *j = body_json(r);
        const char *key = jget_str(j, "system", ""), *id = jget_str(j, "profile", "");
        config_lock();
        int rc = key[0] && id[0] ? choose_system(g_cfg.profiles, key, id) : -1;
        if (rc == 0) {
            g_cfg.profiles_custom = 1;
            rc = config_save();
        }
        config_unlock();
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, "unknown system or emulator");
        else send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/diagnostics")) {
        char *txt = diagnostics_text();
        send_response(fd, 200, "text/plain; charset=utf-8", txt ? txt : "", txt ? strlen(txt) : 0,
                      "Content-Disposition: attachment; filename=\"romm-sync-diagnostics.txt\"\r\n");
        free(txt);
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
        /* Profiles for the emulators picked in the wizard. "choices" maps a
         * system several of them play to the root of its main emulator;
         * "also" lists, per system, the other emulators that keep it as
         * extras (downloads linked in, a save shared where the format is the
         * same); the rest exclude it. */
        cJSON *j = body_json(r), *found = detect_emulators(), *profiles = cJSON_CreateArray();
        const cJSON *root, *c, *choices = cJSON_GetObjectItemCaseSensitive(j, "choices");
        const cJSON *also = cJSON_GetObjectItemCaseSensitive(j, "also");
        cJSON_ArrayForEach(root, cJSON_GetObjectItemCaseSensitive(j, "roots")) {
            cJSON_ArrayForEach(c, found) {
                if (!cJSON_IsString(root) || strcmp(jget_str(c, "root", ""), root->valuestring) != 0) continue;
                if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(c, "ready"))) continue; /* no preset: "custom" below */
                cJSON *p = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(c, "profile"), 1), *ex = cJSON_CreateArray(),
                      *extra = cJSON_CreateArray();
                const cJSON *e, *x;
                cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
                    const char *key = profiles_system_key(e), *keeper = jget_str(choices, key, NULL);
                    if (!keeper || !strcmp(keeper, root->valuestring)) continue;
                    /* "also": {"psx": [roots]} keeps the system on those as extras. */
                    int kept = 0;
                    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(also, key))
                        if (cJSON_IsString(x) && !strcmp(x->valuestring, root->valuestring)) kept = 1;
                    cJSON_AddItemToArray(kept ? extra : ex, cJSON_CreateString(key));
                }
                if (cJSON_GetArraySize(ex)) cJSON_AddItemToObject(p, "exclude", ex);
                else cJSON_Delete(ex);
                if (cJSON_GetArraySize(extra)) cJSON_AddItemToObject(p, "extra", extra);
                else cJSON_Delete(extra);
                cJSON_AddItemToArray(profiles, p);
            }
        }
        /* "custom": folders the user picked, for a system no emulator was found
         * for or for an emulator with no preset yet: [{"slugs": ["snes", "sfam"],
         * "name", "rom_dir", "save_dir", "state_dir", "id"}] ("id" names the
         * profile when it's an emulator's, e.g. "porpoise-ngc"). Saves there are
         * matched by the game's file name. */
        const int presets = cJSON_GetArraySize(profiles);
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j, "custom")) {
            const cJSON *slugs = cJSON_GetObjectItemCaseSensitive(c, "slugs");
            const char *first = cJSON_IsString(cJSON_GetArrayItem(slugs, 0)) ? cJSON_GetArrayItem(slugs, 0)->valuestring : "";
            const char *rom_dir = jget_str(c, "rom_dir", "");
            if (!first[0] || rom_dir[0] != '/') continue;
            char id[96];
            snprintf(id, sizeof id, "custom-%s", jget_str(c, "id", first)[0] ? jget_str(c, "id", first) : first);
            cJSON *p = cJSON_CreateObject(), *plat = cJSON_CreateObject(), *plats = cJSON_CreateArray();
            cJSON_AddStringToObject(p, "id", id);
            cJSON_AddStringToObject(p, "name", jget_str(c, "name", first));
            cJSON_AddBoolToObject(p, "enabled", 1);
            cJSON_AddStringToObject(p, "root", rom_dir);
            cJSON_AddStringToObject(p, "rom_dir", rom_dir);
            const char *save_dir = jget_str(c, "save_dir", ""), *state_dir = jget_str(c, "state_dir", "");
            cJSON_AddStringToObject(p, "save_dir", save_dir[0] == '/' ? save_dir : rom_dir);
            cJSON_AddStringToObject(p, "state_dir", state_dir[0] == '/' ? state_dir : (save_dir[0] == '/' ? save_dir : rom_dir));
            cJSON_AddStringToObject(p, "save_exts", ".srm,.sav,.mcd,.mcr,.eep,.fla,.sra,.dsv");
            cJSON_AddStringToObject(p, "state_exts", ".state*");
            cJSON_AddItemToObject(plat, "romm", cJSON_Duplicate(slugs, 1));
            cJSON_AddStringToObject(plat, "dir", first);
            cJSON_AddItemToArray(plats, plat);
            cJSON_AddItemToObject(p, "platforms", plats);
            /* A system another emulator already plays: this one gets its games
             * too, as an extra, and its saves sync on their own. */
            int played = 0;
            for (int k = 0; k < presets && !played; k++) {
                const cJSON *other = cJSON_GetArrayItem(profiles, k), *e2, *s2;
                cJSON_ArrayForEach(e2, cJSON_GetObjectItemCaseSensitive(other, "platforms")) {
                    if (profiles_excludes(other, profiles_system_key(e2))) continue;
                    cJSON_ArrayForEach(s2, cJSON_GetObjectItemCaseSensitive(e2, "romm")) {
                        const cJSON *mine;
                        cJSON_ArrayForEach(mine, slugs) if (cJSON_IsString(s2) && cJSON_IsString(mine) &&
                                                            !strcmp(s2->valuestring, mine->valuestring)) played = 1;
                    }
                }
            }
            if (played) {
                cJSON *extra = cJSON_AddArrayToObject(p, "extra");
                cJSON_AddItemToArray(extra, cJSON_CreateString(profiles_system_key(plat)));
            }
            cJSON_AddItemToArray(profiles, p);
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
        else {
            library_link_extras(); /* extras get the games already there */
            send_ok(fd);
        }
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
    if (is_get && !strncmp(r->path, "/api/roms/", 10) && strstr(r->path, "/details")) {
        int id = atoi(r->path + 10);
        cJSON *j = id > 0 ? library_details(id, err, sizeof err) : NULL;
        if (j) send_json(fd, 200, j);
        else send_error(fd, id > 0 ? 502 : 400, id > 0 ? err : "no game");
        return;
    }
    if (is_get && !strcmp(r->path, "/api/fs")) {
        char path[PATH_MAX_LEN];
        qparam(r, "path", path, sizeof path);
        cJSON *j = fs_folders(path, err, sizeof err);
        if (j) send_json(fd, 200, j);
        else send_error(fd, 400, err);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/covers/refresh")) {
        covers_refresh();
        send_ok(fd);
        return;
    }
    if (is_get && !strcmp(r->path, "/api/installed")) {
        int limit = qint(r, "limit", 20);
        send_json(fd, 200, library_installed(limit < 1 || limit > 200 ? 20 : limit));
        return;
    }
    if (is_post && !strcmp(r->path, "/api/download")) {
        cJSON *j = body_json(r);
        int rc;
        if (cJSON_GetObjectItemCaseSensitive(j, "rom_id"))
            rc = library_queue_rom((int)jget_num(j, "rom_id", 0), jget_str(j, "name", ""), err, sizeof err);
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
        else send_error(fd, 500, plat_info()->has_tile ? "tile install failed" : "home screen tiles aren't supported on this console");
        return;
    }
    if (is_get && !strcmp(r->path, "/video")) {
        /* RommPS's Codec call: a YouTube video in the console's browser, its
         * player filling the page. YouTube's embeds refuse to play as a page
         * of their own (error 153, no referrer), so this page hosts one. */
        char v[32];
        qparam(r, "v", v, sizeof v);
        if (!v[0] || strspn(v, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(v)) {
            send_error(fd, 400, "no video");
            return;
        }
        char html[1024];
        int n = snprintf(html, sizeof html,
                         "<!doctype html><html><head><meta charset=\"utf-8\"><title>RommPS</title>"
                         "<meta name=\"referrer\" content=\"strict-origin-when-cross-origin\">"
                         "<style>html,body{margin:0;height:100%%;background:#000;overflow:hidden}"
                         "iframe{position:fixed;inset:0;width:100%%;height:100%%;border:0}</style></head><body>"
                         "<iframe src=\"https://www.youtube.com/embed/%s?autoplay=1&rel=0&playsinline=1&fs=1\" "
                         "allow=\"autoplay; fullscreen; encrypted-media; picture-in-picture\" allowfullscreen "
                         "referrerpolicy=\"strict-origin-when-cross-origin\"></iframe></body></html>",
                         v);
        send_response(fd, 200, "text/html; charset=utf-8", html, (size_t)n, NULL);
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
