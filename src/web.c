#include "web.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#include "events.h"
#include "http.h"
#include "library.h"
#include "memcard.h"
#include "pair.h"
#include "platform.h"
#include "presets.h"
#include "profiles.h"
#include "ra2.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "ui_index.h" /* generated from ui/index.html */
#include "update.h"
#include "util.h"
#include "zip.h"

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
    /* For telling our own pages from other sites' (cross_site). */
    char host[256], origin[256], fetch_site[32];
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

/* The UI with the theme already on <html>, so the first frame is right, and
 * the app's name, where its updates come from and what the machine is called
 * where they aren't RommPS's and a console's. */
static void send_page(int fd) {
    char attr[256], hdr[256];
    config_lock();
    snprintf(attr, sizeof attr, " data-theme=\"%s\"", g_cfg.ui_theme);
    config_unlock();
    size_t an = strlen(attr);
    if (plat_info()->product)
        an += (size_t)snprintf(attr + an, sizeof attr - an, " data-product=\"%s\" data-store=\"%s\"", plat_product(),
                               plat_update_source());
    if (plat_info()->here && an < sizeof attr)
        an += (size_t)snprintf(attr + an, sizeof attr - an, " data-here=\"%s\"", plat_info()->here);
    if (plat_info()->peers && an < sizeof attr)
        snprintf(attr + an, sizeof attr - an, " data-peers=\"%s\"", plat_info()->peers);
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

/* A header's value, from just after "Name:" to the end of its line. */
static void header_value(const char *v, char *out, size_t n) {
    while (*v == ' ' || *v == '\t') v++;
    size_t len = strcspn(v, "\r\n");
    while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
    if (len >= n) len = n - 1;
    memcpy(out, v, len);
    out[len] = 0;
}

/* Whether another site's page made the browser send this. Browsers say where
 * a request comes from (Origin, Sec-Fetch-Site); apps such as RommPS and curl
 * send neither, and our own pages come from this very address. */
static int cross_site(const request *r) {
    if (!strcasecmp(r->fetch_site, "cross-site") || !strcasecmp(r->fetch_site, "same-site")) return 1;
    if (!r->origin[0]) return 0;
    const char *o = strstr(r->origin, "://");
    return !o || strcasecmp(o + 3, r->host) != 0; /* "null" included */
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
        else if (!strncasecmp(h + 2, "Host:", 5)) header_value(h + 7, r->host, sizeof r->host);
        else if (!strncasecmp(h + 2, "Origin:", 7)) header_value(h + 9, r->origin, sizeof r->origin);
        else if (!strncasecmp(h + 2, "Sec-Fetch-Site:", 15)) header_value(h + 17, r->fetch_site, sizeof r->fetch_site);
    }
    if (clen > (!strcmp(r->path, "/api/autostart/upload") || !strcmp(r->path, "/api/autostart/install") ? MAX_UPLOAD : MAX_BODY)) {
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
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddNumberToObject(j, "uptime", (double)(time(NULL) - g_started));
    config_lock();
    cJSON_AddBoolToObject(j, "paired", config_is_paired());
    cJSON_AddBoolToObject(j, "setup_complete", g_cfg.setup_complete);
    cJSON_AddBoolToObject(j, "save_sync", g_cfg.save_sync);
    cJSON_AddStringToObject(j, "server_url", g_cfg.server_url);
    cJSON_AddStringToObject(j, "username", g_cfg.username);
    cJSON_AddStringToObject(j, "server_version", g_cfg.server_version);
    cJSON_AddStringToObject(j, "device_id", g_cfg.device_id);
    cJSON_AddNumberToObject(j, "web_port", g_cfg.web_port);
    config_unlock();
    /* RomM not answering: the clients say so (it's checked again meanwhile). */
    char trouble[160];
    if (romm_no_answer(trouble, sizeof trouble)) {
        cJSON_AddStringToObject(j, "server_trouble", trouble);
        romm_check_back();
    }
    pthread_mutex_lock(&g_fg_lock);
    cJSON_AddStringToObject(j, "foreground", g_foreground);
    pthread_mutex_unlock(&g_fg_lock);
    cJSON_AddItemToObject(j, "sync", sync_status_json());
    cJSON_AddItemToObject(j, "pairing", pair_status());
    cJSON_AddItemToObject(j, "covers", covers_status());
    cJSON_AddNumberToObject(j, "installed_sig", library_installed_sig());
    ra2_check(0);
    cJSON *move = ra2_pending();
    if (move) cJSON_AddItemToObject(j, "retroarch_move", move);
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

/* Whether a list in obj ("on": [roots]) names root. */
static int lists_root(const cJSON *obj, const char *list, const char *root) {
    const cJSON *x;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(obj, list))
        if (cJSON_IsString(x) && !strcmp(x->valuestring, root)) return 1;
    return 0;
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
    snprintf(line, sizeof line, "%s diagnostics\nversion %s, build %s, %s\n%s\nclock %s, up %lds\n", plat_product(),
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

/* Diagnostic bundle: a zip with the report, the logs, the sync log, the
 * settings and emulators found, the emulators' own logs and the kernel log
 * when it can be read. Nothing in it names the person: the server, user
 * name, device name and ids, tokens, IP and MAC addresses, emails and
 * console user ids are replaced before anything is written. */
typedef struct { char from[600]; const char *to; } redaction;

static size_t redactions(redaction *r, size_t max) {
    size_t n = 0;
    config_lock();
    const struct { const char *v; const char *to; } src[] = {
        {g_cfg.token, "<token>"}, {g_cfg.server_url, "<server>"}, {g_cfg.username, "<user>"},
        {g_cfg.device_name, "<device>"}, {g_cfg.device_id, "<device-id>"},
        {g_cfg.client_device_identifier, "<device-id>"}};
    for (size_t i = 0; i < sizeof src / sizeof src[0] && n < max; i++)
        if (strlen(src[i].v) >= 3 && strcmp(src[i].v, plat_info()->console)) {
            str_copy(r[n].from, sizeof r[n].from, src[i].v);
            r[n++].to = src[i].to;
        }
    /* The server's host alone, as it shows in errors. */
    const char *h = strstr(g_cfg.server_url, "://");
    if (h && n < max) {
        h += 3;
        size_t l = strcspn(h, ":/");
        if (l >= 3 && l < sizeof r[n].from) {
            memcpy(r[n].from, h, l);
            r[n].from[l] = 0;
            r[n++].to = "<server>";
        }
    }
    config_unlock();
    return n;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_digit(c) || ((c | 32) >= 'a' && (c | 32) <= 'z'); }
static int is_word(char c) { return is_hex(c) || ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_' || c == '.' || c == '-' || c == '+'; }

/* Length of a dotted IPv4 address at s, or 0. */
static size_t ipv4_at(const char *s) {
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        size_t d = 0;
        while (is_digit(s[i + d]) && d < 4) d++;
        if (!d || d > 3) return 0;
        i += d;
        if (part < 3) {
            if (s[i] != '.') return 0;
            i++;
        }
    }
    return is_digit(s[i]) || (s[i] == '.' && is_digit(s[i + 1])) ? 0 : i;
}

/* Length of a MAC address (aa:bb:cc:dd:ee:ff or with -) at s, or 0. */
static size_t mac_at(const char *s) {
    for (int i = 0; i < 6; i++) {
        if (!is_hex(s[i * 3]) || !is_hex(s[i * 3 + 1])) return 0;
        if (i < 5 && s[i * 3 + 2] != ':' && s[i * 3 + 2] != '-') return 0;
    }
    return is_hex(s[17]) ? 0 : 17;
}

/* Hosts every install talks to, which name nobody. */
static int public_host(const char *h, size_t l) {
    static const char *const known[] = {"github.com", "githubusercontent.com", "youtube.com", "localhost"};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        size_t k = strlen(known[i]);
        if (l >= k && !strncasecmp(h + l - k, known[i], k) && (l == k || h[l - k - 1] == '.')) return 1;
    }
    return 0;
}

/* A host name at s: its length, and in *pre the text before it ("https://",
 * or curl's "connect to <host> port" and "resolve host: <host>"); 0 when
 * there's none. The paired server is handled by the redactions; this catches
 * the ones paired before, still in old log lines. */
static size_t host_at(const char *s, size_t *pre) {
    static const struct { const char *before, *after; } forms[] = {
        {"http://", ""}, {"https://", ""}, {"connect to ", " port "}, {"resolve host: ", ""}};
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        size_t b = strlen(forms[i].before), l = 0;
        if (strncasecmp(s, forms[i].before, b)) continue;
        while (is_word(s[b + l]) && s[b + l] != '+') l++;
        while (l && s[b + l - 1] == '.') l--; /* a full stop after it */
        if (!l || strncmp(s + b + l, forms[i].after, strlen(forms[i].after))) continue;
        *pre = b;
        return l;
    }
    return 0;
}

/* A copy of txt with everything that could name the person replaced. */
static char *redact(const char *txt, const redaction *r, size_t nr) {
    textbuf b = {0};
    tb_str(&b, "");
    for (const char *p = txt; *p;) {
        size_t m = 0;
        const char *to = NULL;
        const int start = p == txt || !is_word(p[-1]);
        for (size_t i = 0; i < nr && !m; i++) { /* whole words only */
            size_t l = strlen(r[i].from);
            if ((p == txt || !is_alnum(p[-1])) && !strncasecmp(p, r[i].from, l) && !is_alnum(p[l])) m = l, to = r[i].to;
        }
        size_t pre = 0, hl = !m && start ? host_at(p, &pre) : 0;
        if (hl && !ipv4_at(p + pre) && !public_host(p + pre, hl) && p[pre + hl] != '@') {
            tb_add(&b, p, pre); /* any other server: the prefix stays, the host goes */
            p += pre;
            m = hl, to = "<host>";
        }
        if (!m && start && (m = ipv4_at(p)) && (!strncmp(p, "127.", 4) || !strncmp(p, "0.0.0.0", 7))) m = 0;
        if (m && !to) to = "<ip>";
        if (!m && start && (m = mac_at(p))) to = "<mac>";
        if (!m && !strncmp(p, "/user/home/", 11)) { /* a console user's id */
            size_t l = 0;
            while (is_hex(p[11 + l])) l++;
            if (l) m = 11 + l, to = "/user/home/<user>";
        }
        static const char *const keys[] = {"onlineid", "online_id", "accountid", "account_id", "npid", "userid", "user_id", "username", "user_name", "email"};
        for (size_t i = 0; !m && start && i < sizeof keys / sizeof keys[0]; i++) { /* key=value, key: "value" */
            size_t l = strlen(keys[i]);
            if (strncasecmp(p, keys[i], l) || is_alnum(p[l])) continue;
            size_t v = l;
            while (p[v] == ' ' || p[v] == '=' || p[v] == ':' || p[v] == '"' || p[v] == '\'' || p[v] == '\t') v++;
            if (v == l || !is_word(p[v])) continue;
            tb_add(&b, p, v);
            m = v;
            while (is_word(p[m])) m++;
            m -= v;
            p += v;
            to = "<id>";
        }
        if (!m && *p == '@' && p > txt && is_word(p[-1]) && is_word(p[1])) { /* an email: drop both sides */
            while (b.len && is_word(b.s[b.len - 1])) b.s[--b.len] = 0;
            m = 1;
            while (is_word(p[m])) m++;
            to = "<email>";
        }
        if (m) {
            tb_str(&b, to);
            p += m;
        } else tb_add(&b, p++, 1);
    }
    return b.s;
}

/* Writes name into dir with its text redacted; adds it to the members. */
static void bundle_add(const char *dir, const char *name, const char *txt, const redaction *r, size_t nr,
                       zip_member *mem, char (*paths)[PATH_MAX_LEN], int *n, int max) {
    if (!txt || *n >= max) return;
    char *clean = redact(txt, r, nr);
    path_join(paths[*n], PATH_MAX_LEN, dir, name);
    if (clean && !write_file_atomic(paths[*n], clean, strlen(clean))) {
        mem[*n].path = paths[*n];
        mem[*n].name = strrchr(paths[*n], '/') + 1;
        (*n)++;
    }
    free(clean);
}

/* The last max bytes of a file, from the start of a line, or NULL. */
static char *file_tail(const char *path, size_t max) {
    size_t len = 0;
    char *txt = read_file(path, &len);
    if (!txt || len <= max) return txt;
    const char *start = txt + len - max, *nl = strchr(start, '\n');
    if (nl) start = nl + 1;
    memmove(txt, start, strlen(start) + 1);
    return txt;
}

/* What the kernel log holds right now, without waiting for more. */
static char *kernel_log(void) {
    int fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    textbuf b = {0};
    char buf[4096];
    time_t until = time(NULL) + 2;
    while (b.len < 512 * 1024 && time(NULL) <= until) {
        ssize_t got = read(fd, buf, sizeof buf);
        if (got <= 0) break;
        tb_add(&b, buf, (size_t)got);
    }
    close(fd);
    return b.s;
}

/* The emulators' own logs: *.log files in each emulator's folder. */
static void emulator_logs(const char *dir, const redaction *r, size_t nr, zip_member *mem,
                          char (*paths)[PATH_MAX_LEN], int *n, int max) {
    cJSON *found = detect_emulators(), *c;
    char seen[16][PATH_MAX_LEN];
    int nseen = 0;
    cJSON_ArrayForEach(c, found) {
        char roots[2][PATH_MAX_LEN] = {"", ""};
        str_copy(roots[0], PATH_MAX_LEN, jget_str(c, "root", ""));
        const char *tid = jget_str(c, "title_id", "");
        if (tid[0]) snprintf(roots[1], PATH_MAX_LEN, "/data/homebrew/%s", tid);
        for (int k = 0; k < 2; k++) {
            int dup = !roots[k][0];
            for (int i = 0; i < nseen && !dup; i++) dup = !strcmp(seen[i], roots[k]);
            if (dup || nseen >= 16) continue;
            str_copy(seen[nseen++], PATH_MAX_LEN, roots[k]);
            DIR *d = opendir(roots[k]);
            struct dirent *de;
            while (d && (de = readdir(d))) {
                if (de->d_name[0] == '.' || !str_ends_with_ci(de->d_name, ".log")) continue;
                char path[PATH_MAX_LEN], name[256];
                path_join(path, sizeof path, roots[k], de->d_name);
                if (dir_exists(path)) continue;
                const char *base = strrchr(roots[k], '/');
                snprintf(name, sizeof name, "emulators-%s-%s", base ? base + 1 : roots[k], de->d_name);
                char *txt = file_tail(path, 256 * 1024);
                bundle_add(dir, name, txt, r, nr, mem, paths, n, max);
                free(txt);
            }
            if (d) closedir(d);
        }
    }
    cJSON_Delete(found);
}

/* Builds the bundle at zip_path. 0 on success. */
static int diagnostics_bundle(const char *zip_path) {
    enum { kMax = 48 };
    redaction r[8];
    size_t nr = redactions(r, 8);
    char dir[PATH_MAX_LEN], path[PATH_MAX_LEN];
    path_join(dir, sizeof dir, plat_data_dir(), "diagnostics.tmp");
    remove_tree(dir);
    mkdir_p(dir);
    zip_member mem[kMax];
    char (*paths)[PATH_MAX_LEN] = malloc(kMax * sizeof *paths); /* console threads have small stacks */
    int n = 0;
    if (!paths) return -1;

    char *txt = diagnostics_text();
    bundle_add(dir, "report.txt", txt, r, nr, mem, paths, &n, kMax);
    free(txt);
    const char *logs[][2] = {{"romm-sync.log", "romm-sync.log"}, {"romm-sync.log.1", "romm-sync.previous.log"},
                             {"events.json", "sync-log.json"}, {"boot.log", "boot.log"}, {"launcher.log", "launcher.log"}};
    for (size_t i = 0; i < sizeof logs / sizeof logs[0]; i++) {
        path_join(path, sizeof path, plat_data_dir(), logs[i][0]);
        txt = read_file(path, NULL);
        bundle_add(dir, logs[i][1], txt, r, nr, mem, paths, &n, kMax);
        free(txt);
    }
    cJSON *j = detect_emulators();
    txt = cJSON_Print(j);
    cJSON_Delete(j);
    bundle_add(dir, "emulators-found.json", txt, r, nr, mem, paths, &n, kMax);
    free(txt);
    config_lock();
    txt = cJSON_Print(g_cfg.profiles);
    config_unlock();
    bundle_add(dir, "emulator-settings.json", txt, r, nr, mem, paths, &n, kMax);
    free(txt);
    emulator_logs(dir, r, nr, mem, paths, &n, kMax);
    txt = kernel_log();
    bundle_add(dir, "kernel.log", txt ? txt : "The kernel log couldn't be read (another tool may be reading it).\n", r,
               nr, mem, paths, &n, kMax);
    free(txt);

    int rc = zip_write(zip_path, mem, n);
    remove_tree(dir);
    free(paths);
    LOGI("diagnostic bundle %s: %d files%s", zip_path, n, rc ? ", failed" : "");
    return rc;
}

/* What bundles are named after: the app, in lower case ("rommps-diagnostics",
 * "rommdeck-diagnostics"). */
static void diagnostics_prefix(char *out, size_t n) {
    const char *app = plat_product();
    size_t i = 0;
    for (; app[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)app[i]);
    snprintf(out + i, n - i, "-diagnostics");
}

/* Saves a bundle where the person can get at it: a USB drive when one is in,
 * and the payload's folder. The newest three are kept there. */
static cJSON *diagnostics_save(void) {
    char stamp[32], prefix[32], name[80], path[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    diagnostics_prefix(prefix, sizeof prefix);
    snprintf(name, sizeof name, "%s-%s.zip", prefix, stamp);
    path_join(dir, sizeof dir, plat_data_dir(), "diagnostics");
    mkdir_p(dir);
    path_join(path, sizeof path, dir, name);
    if (diagnostics_bundle(path)) return NULL;
    /* Older ones go: names sort by time. */
    DIR *d = opendir(dir);
    struct dirent *de;
    char (*old)[64] = malloc(64 * sizeof *old);
    int n = 0;
    size_t pl = strlen(prefix);
    while (old && d && (de = readdir(d)) && n < 64) {
        if (!strncmp(de->d_name, prefix, pl) && de->d_name[pl] == '-') {
            str_copy(old[n++], sizeof old[0], de->d_name);
        } else if (strcmp(prefix, "rommps-diagnostics") != 0 && !strncmp(de->d_name, "rommps-diagnostics-", 19)) {
            /* one from before RommDeck named them after itself */
            char p[PATH_MAX_LEN];
            path_join(p, sizeof p, dir, de->d_name);
            unlink(p);
        }
    }
    if (d) closedir(d);
    if (old) {
        qsort(old, (size_t)n, sizeof old[0], (int (*)(const void *, const void *))strcmp);
        for (int i = 0; i + 3 < n; i++) {
            char p[PATH_MAX_LEN];
            path_join(p, sizeof p, dir, old[i]);
            unlink(p);
        }
        free(old);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "path", path);
    const char *usb[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3"};
    for (size_t i = 0; i < sizeof usb / sizeof usb[0]; i++) {
        char to[PATH_MAX_LEN];
        path_join(to, sizeof to, usb[i], name);
        if (dir_exists(usb[i]) && !copy_file(path, to)) {
            cJSON_AddStringToObject(j, "usb", to);
            break;
        }
    }
    return j;
}

/* Covers and platform icons, from the console's cache or RomM (covers.h). */
static void proxy_asset(const request *r) {
    char p[1024];
    qparam(r, "p", p, sizeof p);
    char *body = NULL;
    size_t len = 0;
    const char *type = "image/jpeg";
    int rc = covers_fetch(p, &body, &len, &type);
    /* Only a cover RomM doesn't have is a 404: RommPS takes that as final,
     * and tries anything else again (RomM busy, a timeout). */
    if (rc == 0) send_response(r->fd, 200, type, body, len, "Cache-Control: max-age=86400\r\n");
    else if (rc == 1) send_error(r->fd, 404, "asset unavailable");
    else if (rc == -2) send_error(r->fd, 400, "bad asset path");
    else send_error(r->fd, 502, "RomM didn't answer for that asset");
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
        snprintf(err, (size_t)en, "not a folder the %s can use", plat_here());
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
    /* Only our own pages and apps change anything: a page on another site
     * could otherwise post here from any browser on the network. */
    if (!is_get && strcmp(r->method, "HEAD") && cross_site(r)) {
        LOGW("refused %s %s from %s", r->method, r->path, r->origin[0] ? r->origin : "another site");
        send_error(fd, 403, "requests from other websites aren't accepted");
        return;
    }
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
        cJSON *j = diagnostics_save();
        size_t len = 0;
        char *zip = j ? read_file(jget_str(j, "path", ""), &len) : NULL;
        char prefix[32], disposition[96];
        diagnostics_prefix(prefix, sizeof prefix);
        snprintf(disposition, sizeof disposition, "Content-Disposition: attachment; filename=\"%s.zip\"\r\n", prefix);
        if (zip) send_response(fd, 200, "application/zip", zip, len, disposition);
        else send_error(fd, 500, "couldn't make the bundle");
        free(zip);
        cJSON_Delete(j);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/diagnostics")) {
        cJSON *j = diagnostics_save();
        if (j) send_json(fd, 200, j);
        else send_error(fd, 500, "couldn't make the bundle");
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
        const int profiles = cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(j, "profiles"));
        config_lock();
        config_apply_json(j);
        http_set_tls_verify(g_cfg.tls_verify);
        int rc = config_save();
        config_unlock();
        cJSON_Delete(j);
        library_wake();
        if (profiles && !rc) library_link_extras(); /* the folders follow the new profiles */
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
    if (is_get && !strcmp(r->path, "/api/presets")) {
        send_json(fd, 200, presets_status());
        return;
    }
    if (is_post && !strcmp(r->path, "/api/presets/check")) {
        if (presets_check() != 0) send_error(fd, 409, "a presets check is already running");
        else send_json(fd, 200, presets_status());
        return;
    }
    /* Autostart works before pairing too: RommPS sets it up and keeps it
     * current whether or not RomM Sync is paired yet. */
    if (is_get && !strcmp(r->path, "/api/autostart")) {
        send_json(fd, 200, autostart_status());
        return;
    }
    /* Turns it on, installing the payload sent along if there is one (RommPS
     * and the web UI's upload). There's no off; older apps asking for one
     * get it turned on, which is what they need anyway. "install" is 1.2's
     * name for it. */
    if (is_post && (!strcmp(r->path, "/api/autostart") || !strcmp(r->path, "/api/autostart/upload") ||
                    !strcmp(r->path, "/api/autostart/install"))) {
        int has_elf = strcmp(r->path, "/api/autostart") != 0;
        if (autostart_enable(has_elf ? r->body : NULL, has_elf ? r->body_len : 0, err, sizeof err)) send_error(fd, 400, err);
        else send_json(fd, 200, autostart_status());
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
         * same); the rest exclude it. "systems" does the same per system,
         * and wins over both: {"ps2": {"main": root, "on": [roots],
         * "folders": {root: {"rom_dir", "save_dir", "state_dir"}}}}. An
         * emulator not "on" excludes it; folders go on that system's entry. */
        cJSON *j = body_json(r), *found = detect_emulators(), *profiles = cJSON_CreateArray();
        const cJSON *root, *c, *choices = cJSON_GetObjectItemCaseSensitive(j, "choices");
        const cJSON *also = cJSON_GetObjectItemCaseSensitive(j, "also");
        const cJSON *systems = cJSON_GetObjectItemCaseSensitive(j, "systems");
        cJSON_ArrayForEach(root, cJSON_GetObjectItemCaseSensitive(j, "roots")) {
            cJSON_ArrayForEach(c, found) {
                if (!cJSON_IsString(root) || strcmp(jget_str(c, "root", ""), root->valuestring) != 0) continue;
                if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(c, "ready"))) continue; /* no preset: "custom" below */
                cJSON *p = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(c, "profile"), 1), *ex = cJSON_CreateArray(),
                      *extra = cJSON_CreateArray();
                cJSON *e;
                cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
                    const char *key = profiles_system_key(e), *keeper = jget_str(choices, key, NULL);
                    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(systems, key);
                    if (sys) {
                        if (!lists_root(sys, "on", root->valuestring)) {
                            cJSON_AddItemToArray(ex, cJSON_CreateString(key));
                            continue;
                        }
                        if (strcmp(jget_str(sys, "main", ""), root->valuestring) != 0) /* on, not the main one */
                            cJSON_AddItemToArray(extra, cJSON_CreateString(key));
                        const cJSON *f = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(sys, "folders"),
                                                                          root->valuestring);
                        static const char *const dirs[] = {"rom_dir", "save_dir", "state_dir"};
                        for (int k = 0; f && k < 3; k++) {
                            const char *d = jget_str(f, dirs[k], "");
                            if (d[0] != '/') continue; /* empty: the default */
                            cJSON_DeleteItemFromObjectCaseSensitive(e, dirs[k]);
                            cJSON_AddStringToObject(e, dirs[k], d);
                        }
                        continue;
                    }
                    if (!keeper || !strcmp(keeper, root->valuestring)) continue;
                    /* "also": {"psx": [roots]} keeps the system on those as extras. */
                    cJSON_AddItemToArray(lists_root(also, key, root->valuestring) ? extra : ex, cJSON_CreateString(key));
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
        /* Save sync per platform, on each platform entry ("sync": "off" or
         * "saves"; none is everything): "systems"{key}{"sync"} from the app,
         * else "sync"{root} (the web UI, per emulator), else what it was. */
        const cJSON *modes = cJSON_GetObjectItemCaseSensitive(j, "sync");
        config_lock();
        cJSON *pr;
        cJSON_ArrayForEach(pr, profiles) {
            const char *at = jget_str(pr, "root", "");
            const cJSON *old = NULL, *o;
            cJSON_ArrayForEach(o, g_cfg.profiles) if (!strcmp(jget_str(o, "root", ""), at) &&
                                                      !strcmp(jget_str(o, "id", ""), jget_str(pr, "id", "")))
                old = o;
            cJSON *pe;
            cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(pr, "platforms")) {
                const char *key = profiles_system_key(pe);
                const char *mode = jget_str(cJSON_GetObjectItemCaseSensitive(systems, key), "sync", NULL);
                if (!mode) mode = jget_str(modes, at, NULL);
                const cJSON *oe;
                cJSON_ArrayForEach(oe, cJSON_GetObjectItemCaseSensitive(old, "platforms")) if (!mode && !strcmp(profiles_system_key(oe), key))
                    mode = jget_str(oe, "sync", NULL);
                if (!mode) mode = jget_str(old, "sync", NULL);
                char keep[8] = "";
                if (mode && (!strcmp(mode, "off") || !strcmp(mode, "saves"))) str_copy(keep, sizeof keep, mode);
                cJSON_DeleteItemFromObjectCaseSensitive(pe, "sync");
                if (keep[0]) cJSON_AddStringToObject(pe, "sync", keep);
            }
            cJSON_DeleteItemFromObjectCaseSensitive(pr, "sync");
        }
        config_unlock();
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
    if (is_post && !strcmp(r->path, "/api/emulators/own-games")) {
        /* An extra's games of its own, in the way of its mount: {"root",
         * "system", "action": "move" (to the main emulator) | "delete"}. */
        cJSON *j = body_json(r);
        char err[200] = "";
        int n = library_own_games(jget_str(j, "root", ""), jget_str(j, "system", ""), jget_str(j, "action", ""), err,
                                  sizeof err);
        cJSON_Delete(j);
        if (n < 0) {
            send_error(fd, 400, err);
        } else {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "games", n);
            send_json(fd, 200, o);
        }
        return;
    }
    if (is_post && !strcmp(r->path, "/api/emulators/sync")) {
        /* One emulator's save sync, for every platform it plays: {"root", "sync": "off" | "saves" | "all"}. */
        cJSON *j = body_json(r), *p;
        const char *at = jget_str(j, "root", ""), *mode = jget_str(j, "sync", "");
        int found = 0, rc = -1;
        if (at[0] && (!strcmp(mode, "off") || !strcmp(mode, "saves") || !strcmp(mode, "all"))) {
            config_lock();
            cJSON_ArrayForEach(p, g_cfg.profiles) {
                if (strcmp(jget_str(p, "root", ""), at) != 0) continue;
                found = 1;
                cJSON_DeleteItemFromObjectCaseSensitive(p, "sync");
                cJSON *pe; /* every platform it plays */
                cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
                    cJSON_DeleteItemFromObjectCaseSensitive(pe, "sync");
                    if (strcmp(mode, "all") != 0) cJSON_AddStringToObject(pe, "sync", mode);
                }
            }
            if (found) {
                g_cfg.profiles_custom = 1;
                rc = config_save();
            }
            config_unlock();
        }
        if (!found) send_error(fd, 400, "unknown emulator or choice");
        else if (rc) send_error(fd, 500, "could not save config");
        else {
            LOGI("save sync for %s: %s", at, mode);
            send_ok(fd);
        }
        cJSON_Delete(j); /* at and mode are in it */
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
    if (is_post && !strcmp(r->path, "/api/notify")) {
        /* RommPS's own messages, as the console's notifications: an app can't send those. */
        cJSON *j = body_json(r);
        const char *message = jget_str(j, "message", "");
        if (message[0]) plat_notify("%s: %s", plat_product(), message);
        cJSON_Delete(j);
        send_ok(fd);
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
    if (is_get && !strcmp(r->path, "/api/events")) {
        send_json(fd, 200, events_json(qint(r, "rom_id", 0), qint(r, "limit", 200)));
        return;
    }
    if (is_get && !strcmp(r->path, "/api/tracked")) {
        send_json(fd, 200, sync_tracked(qint(r, "rom_id", 0)));
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
    if (is_post && !strcmp(r->path, "/api/retroarch/move")) {
        int moved = 0;
        int rc = ra2_move_games(&moved, err, sizeof err);
        if (rc && !moved) {
            send_error(fd, 400, err);
            return;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "moved", moved);
        if (rc) cJSON_AddStringToObject(j, "error", err);
        send_json(fd, 200, j);
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
    if (is_get && !strcmp(r->path, "/api/collections")) {
        /* Every kind offered, or only kind's. */
        char kind[32];
        collection_kind k = COLLECTION_NONE;
        if (qparam(r, "kind", kind, sizeof kind) && (k = library_collection_kind(kind)) == COLLECTION_NONE) {
            send_error(fd, 400, "unknown kind of collection");
            return;
        }
        int refused = 0;
        cJSON *j = library_collections(k, &refused, err, sizeof err);
        if (j) send_json(fd, 200, j);
        else send_error(fd, refused ? 403 : 502, err); /* 403: pair again */
        return;
    }
    if (is_get && !strcmp(r->path, "/api/roms")) {
        /* platform_id narrows to a platform (0 or none: every one); collection_id
         * to a collection, of collection_kind (regular unless said). */
        char search[256], kind[32];
        library_scope scope = {.platform_id = qint(r, "platform_id", 0), .collection = COLLECTION_NONE};
        qparam(r, "search", search, sizeof search);
        if (qparam(r, "collection_id", scope.collection_id, sizeof scope.collection_id) && scope.collection_id[0]) {
            if (!qparam(r, "collection_kind", kind, sizeof kind)) str_copy(kind, sizeof kind, "regular");
            scope.collection = library_collection_kind(kind);
            if (scope.collection == COLLECTION_NONE) {
                send_error(fd, 400, "unknown kind of collection");
                return;
            }
            if (!library_collection_id_ok(scope.collection, scope.collection_id)) {
                send_error(fd, 400, "no such collection");
                return;
            }
        }
        int limit = qint(r, "limit", 60);
        if (limit < 1 || limit > 500) limit = 60;
        cJSON *j = library_roms(&scope, search, qint(r, "offset", 0), limit, err, sizeof err);
        if (j) send_json(fd, 200, j);
        else send_error(fd, 502, err);
        return;
    }
    if (is_get && !strncmp(r->path, "/api/roms/", 10) && strstr(r->path, "/on_console")) {
        int id = atoi(r->path + 10);
        if (id <= 0) {
            send_error(fd, 400, "no game");
            return;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "installed", library_on_console(id));
        cJSON *discs = library_discs_here(id);
        if (discs) cJSON_AddItemToObject(j, "discs", discs);
        send_json(fd, 200, j);
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
        send_json(fd, 200, library_installed(limit < 1 ? 20 : limit > 5000 ? 5000 : limit));
        return;
    }
    if (is_post && !strcmp(r->path, "/api/download")) {
        cJSON *j = body_json(r);
        int rc;
        if (cJSON_GetObjectItemCaseSensitive(j, "collection_id")) {
            /* A whole collection: every game of it not here yet. */
            library_scope scope = {.collection = library_collection_kind(jget_str(j, "collection_kind", "regular"))};
            str_copy(scope.collection_id, sizeof scope.collection_id, jget_str(j, "collection_id", ""));
            int queued = 0, skipped = 0;
            if (!library_collection_id_ok(scope.collection, scope.collection_id)) {
                snprintf(err, sizeof err, "no such collection");
                rc = -1;
            } else {
                rc = library_queue_scope(&scope, &queued, &skipped, err, sizeof err);
            }
            cJSON_Delete(j);
            if (rc) {
                send_error(fd, 400, err);
            } else {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddNumberToObject(o, "queued", queued);
                cJSON_AddNumberToObject(o, "skipped", skipped);
                send_json(fd, 200, o);
            }
            return;
        }
        if (cJSON_GetObjectItemCaseSensitive(j, "rom_id"))
            rc = library_queue_rom((int)jget_num(j, "rom_id", 0), (int)jget_num(j, "disc", 0), jget_str(j, "name", ""),
                                   jget_str(j, "cover", ""), err, sizeof err);
        else
            rc = library_queue_firmware((int)jget_num(j, "platform_id", 0), err, sizeof err);
        cJSON_Delete(j);
        if (rc) send_error(fd, 400, err);
        else send_ok(fd);
        return;
    }
    if (is_post && !strcmp(r->path, "/api/delete")) {
        cJSON *j = body_json(r);
        const int disc = (int)jget_num(j, "disc", 0);
        int rc = disc > 0 ? library_delete_disc((int)jget_num(j, "rom_id", 0), disc, err, sizeof err)
                          : library_delete_rom((int)jget_num(j, "rom_id", 0), err, sizeof err);
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
            if (maps[i].twin) continue; /* (its entry's core's saves, shown with it) */
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "profile", maps[i].profile_name);
            cJSON_AddStringToObject(o, "platforms", maps[i].romm_slugs);
            cJSON_AddStringToObject(o, "emulator", maps[i].emulator);
            cJSON_AddStringToObject(o, "rom_dir", maps[i].rom_dir);
            cJSON_AddStringToObject(o, "root", maps[i].root);
            cJSON_AddStringToObject(o, "save_dir", maps[i].sync_saves ? maps[i].save_dir : "");
            cJSON_AddStringToObject(o, "state_dir", maps[i].sync_saves && maps[i].sync_states ? maps[i].state_dir : "");
            cJSON_AddStringToObject(o, "bios_dir", maps[i].bios_dir);
            cJSON_AddStringToObject(o, "sync_note", maps[i].sync_note);
            cJSON_AddStringToObject(o, "save_layout", maps[i].save_layout);
            /* Its memory cards: one per game or shared, and the shared ones here. */
            if (maps[i].cards_mode[0]) {
                cJSON *cards = cJSON_AddObjectToObject(o, "cards");
                cJSON_AddStringToObject(cards, "mode", maps[i].cards_mode);
                cJSON_AddStringToObject(cards, "method", maps[i].cards_method); /* the system's, one at a time */
                cJSON_AddBoolToObject(cards, "active", maps[i].cards_active);
                cJSON_AddStringToObject(cards, "default", maps[i].cards_default);
                cJSON_AddStringToObject(cards, "modes", maps[i].cards_modes);
                cJSON_AddStringToObject(cards, "change", maps[i].cards_change);
                cJSON *found = cJSON_AddArrayToObject(cards, "shared_here");
                char list[sizeof maps[i].memcards], *save = NULL;
                str_copy(list, sizeof list, maps[i].memcards);
                for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save))
                    if (file_exists(t)) cJSON_AddItemToArray(found, cJSON_CreateString(t));
            }
            /* An extra playing the main emulator's games through a mount:
             * its own folder, and where the mount goes in its sandbox. */
            if (maps[i].mount_title[0]) {
                cJSON_AddStringToObject(o, "own_rom_dir", maps[i].own_rom_dir);
                cJSON_AddStringToObject(o, "mount", maps[i].mount_into);
            }
            cJSON_AddStringToObject(o, "mount_note", maps[i].mount_note);
            char sys[64];
            str_copy(sys, sizeof sys, maps[i].romm_slugs);
            sys[strcspn(sys, ",")] = 0;
            cJSON_AddStringToObject(o, "system", sys);
            cJSON_AddBoolToObject(o, "extra", maps[i].extra);
            cJSON_AddNumberToObject(o, "own_games", maps[i].mount_blocked);
            cJSON_AddItemToArray(arr, o);
        }
        profiles_free(maps);
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
    if (is_post && !strcmp(r->path, "/api/downloads/retry")) {
        cJSON *j = body_json(r);
        int rc = library_retry((int)jget_num(j, "id", 0), err, sizeof err);
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
                         "<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title>"
                         "<meta name=\"referrer\" content=\"strict-origin-when-cross-origin\">"
                         "<style>html,body{margin:0;height:100%%;background:#000;overflow:hidden}"
                         "iframe{position:fixed;inset:0;width:100%%;height:100%%;border:0}</style></head><body>"
                         "<iframe src=\"https://www.youtube.com/embed/%s?autoplay=1&rel=0&playsinline=1&fs=1\" "
                         "allow=\"autoplay; fullscreen; encrypted-media; picture-in-picture\" allowfullscreen "
                         "referrerpolicy=\"strict-origin-when-cross-origin\"></iframe></body></html>",
                         plat_product(), v);
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
