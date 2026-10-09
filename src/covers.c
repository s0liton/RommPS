/* Covers and platform icons on the console. See covers.h. */
#include "covers.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "config.h"
#include "http.h"
#include "library.h"
#include "platform.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "util.h"

/* An asset RomM doesn't have is remembered for a day (a .missing file), so it
 * isn't asked for again on every page. The folder is pruned, oldest first,
 * past its limit: room for a whole library's covers on the PS5 (66 KB each on
 * average), less where storage is tight. The full pass stops short of it. */
#define MISSING_SEC (24 * 60 * 60)
#define FULL_EVERY_SEC (24 * 60 * 60)
#define WARM_SLOTS 256

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static int g_prune; /* the cache went past its limit */
static long long g_cached_bytes = -1; /* -1 until counted */

/* Covers to fetch soon (the pages being looked at), newest first. */
static char g_warm[WARM_SLOTS][512];
static int g_nwarm;

/* The full pass: every game's cover path, worked through by the workers. */
static char **g_all;
static int g_nall, g_next, g_done;
static int g_refresh; /* a full pass was asked for */
static char g_state[16] = "idle";

static int is_ps5(void) { return strcmp(plat_info()->name, "ps5") == 0; }

static long long cache_limit(void) { return is_ps5() ? 1024LL * 1024 * 1024 : 300LL * 1024 * 1024; }

static int workers(void) { return is_ps5() ? 4 : 2; }

static void cache_path(const char *p, const char *suffix, char *out, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a of the asset path and query */
    for (const unsigned char *c = (const unsigned char *)p; *c; c++) h = (h ^ *c) * 0x100000001b3ULL;
    snprintf(out, n, "%s/cache/assets/%016llx%s", plat_data_dir(), (unsigned long long)h, suffix);
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

/* RomM's platform icons only carry a viewBox, and the console browser renders an
 * SVG without a size at 0x0: give it the viewBox's size. */
static char *svg_with_size(const char *svg, size_t len, size_t *out_len) {
    const char *tag = strstr(svg, "<svg");
    if (!tag) return NULL;
    const char *end = strchr(tag, '>');
    if (!end) return NULL;
    for (const char *q = tag; q < end; q++)
        if (!strncmp(q, " width=", 7)) return NULL; /* already sized */
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

typedef struct {
    time_t mtime;
    long long size;
    char name[200];
} cached_file;

static int by_age(const void *a, const void *b) {
    time_t x = ((const cached_file *)a)->mtime, y = ((const cached_file *)b)->mtime;
    return x < y ? -1 : x > y;
}

/* The folder's size; and, past the limit, the oldest files deleted until it's
 * back under 95% of it. One pass over the folder, on the planner's thread. */
static long long cache_scan(int prune) {
    char dir[PATH_MAX_LEN], path[PATH_MAX_LEN];
    snprintf(dir, sizeof dir, "%s/cache/assets", plat_data_dir());
    DIR *d = opendir(dir);
    if (!d) return 0;
    cached_file *files = NULL;
    int n = 0, cap = 0;
    long long total = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        struct stat st;
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof files->name) continue;
        path_join(path, sizeof path, dir, de->d_name);
        if (stat(path, &st) != 0) continue;
        total += st.st_size;
        if (!prune) continue;
        if (n == cap) {
            cached_file *grown = realloc(files, (size_t)(cap = cap ? cap * 2 : 1024) * sizeof *files);
            if (!grown) break;
            files = grown;
        }
        files[n].mtime = st.st_mtime;
        files[n].size = st.st_size;
        str_copy(files[n].name, sizeof files[n].name, de->d_name);
        n++;
    }
    closedir(d);
    if (prune && total > cache_limit()) {
        qsort(files, (size_t)n, sizeof *files, by_age);
        for (int i = 0; i < n && total > cache_limit() / 20 * 19; i++) {
            path_join(path, sizeof path, dir, files[i].name);
            if (unlink(path) == 0) total -= files[i].size;
        }
    }
    free(files);
    pthread_mutex_lock(&g_lock);
    g_cached_bytes = total;
    pthread_mutex_unlock(&g_lock);
    return total;
}

static void cache_store(const char *path, const void *body, size_t len) {
    char tmp[PATH_MAX_LEN + 32];
    snprintf(tmp, sizeof tmp, "%s.%lx.tmp", path, (unsigned long)pthread_self());
    mkdir_parent(path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(body, 1, len, f) == len;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_cached_bytes >= 0) g_cached_bytes += (long long)len;
    if (g_cached_bytes > cache_limit()) g_prune = 1; /* the planner prunes, not a request's thread */
    pthread_mutex_unlock(&g_lock);
}

static int valid_path(const char *p) { return p && !strncmp(p, "/assets/", 8) && !strstr(p, ".."); }

/* Whether an asset is on the console (or known to be missing on RomM). */
static int have(const char *p) {
    char cached[PATH_MAX_LEN], missing[PATH_MAX_LEN];
    cache_path(p, "", cached, sizeof cached);
    if (file_exists(cached)) return 1;
    cache_path(p, ".missing", missing, sizeof missing);
    time_t miss = file_mtime(missing);
    return miss && time(NULL) - miss < MISSING_SEC;
}

int covers_fetch(const char *p, char **out, size_t *out_len, const char **type) {
    char base[512], auth[300], url[3200], cached[PATH_MAX_LEN], missing[PATH_MAX_LEN];
    if (!valid_path(p)) return -1;
    *type = image_type(p);
    cache_path(p, "", cached, sizeof cached);
    cache_path(p, ".missing", missing, sizeof missing);
    size_t clen = 0;
    char *cbody = read_file(cached, &clen);
    if (cbody) {
        *out = cbody;
        *out_len = clen;
        return 0;
    }
    time_t miss = file_mtime(missing);
    if (miss && time(NULL) - miss < MISSING_SEC) return 1;
    config_lock();
    str_copy(base, sizeof base, g_cfg.server_url);
    snprintf(auth, sizeof auth, "Bearer %s", g_cfg.token);
    config_unlock();
    /* Cover paths come with literal spaces in them ("?ts=2026-08-03 07:20:30"),
     * which libcurl refuses: they get encoded. */
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
        int gone = resp.status == 404;
        if (gone) cache_store(missing, "", 0);
        http_resp_free(&resp);
        return gone ? 1 : -1;
    }
    unlink(missing);
    size_t len = resp.len;
    char *fixed = !strcmp(*type, "image/svg+xml") ? svg_with_size(resp.body, len, &len) : NULL;
    const char *body = fixed ? fixed : resp.body;
    cache_store(cached, body, len);
    *out = malloc(len + 1);
    if (*out) {
        memcpy(*out, body, len);
        (*out)[len] = 0;
        *out_len = len;
    }
    free(fixed);
    http_resp_free(&resp);
    return *out ? 0 : -1;
}

void covers_warm(const char *p) {
    if (!valid_path(p) || have(p)) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nwarm; i++)
        if (!strcmp(g_warm[i], p)) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    /* The newest first; the oldest drop off a full queue. */
    if (g_nwarm == WARM_SLOTS) g_nwarm--;
    memmove(g_warm[1], g_warm[0], sizeof g_warm[0] * (size_t)g_nwarm);
    str_copy(g_warm[0], sizeof g_warm[0], p);
    g_nwarm++;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

void covers_refresh(void) {
    pthread_mutex_lock(&g_lock);
    g_refresh = 1;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

static int enabled(void) {
    config_lock();
    int on = g_cfg.cover_cache && config_is_paired() && g_cfg.setup_complete;
    config_unlock();
    return on;
}

/* The console is busy with something that matters more. */
static int busy(void) { return sync_game_running() || sync_is_running() || library_busy(); }

static int collect_cover(const cJSON *it, const cJSON *page, int first, void *ctx) {
    (void)page, (void)first;
    if (!it) return 0;
    const char *p = jget_str(it, "path_cover_small", "");
    if (!valid_path(p)) return 0;
    struct { char **list; int n, cap; } *c = ctx;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 512;
        char **grown = realloc(c->list, sizeof *grown * (size_t)c->cap);
        if (!grown) return -1;
        c->list = grown;
    }
    c->list[c->n++] = strdup(p);
    return 0;
}

/* Every game's cover path, platform by platform. 0 when RomM gave the whole
 * list; -1 if it couldn't be reached part way (what came is still used). */
static int list_all(void) {
    struct { char **list; int n, cap; } c = {0};
    long st = 0;
    cJSON *plats = romm_call("GET", "/api/platforms", NULL, &st), *p;
    int ok = st == 200 && cJSON_IsArray(plats);
    cJSON_ArrayForEach(p, plats) {
        if (jget_num(p, "rom_count", 0) <= 0) continue;
        char path[160];
        snprintf(path, sizeof path, "/api/roms?platform_ids=%d&order_by=name&order_dir=asc&with_char_index=false" ROMS_LEAN, (int)jget_num(p, "id", 0));
        if (romm_list(path, 0, 100000, collect_cover, &c, &st) < 0) ok = 0;
    }
    cJSON_Delete(plats);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nall; i++) free(g_all[i]);
    free(g_all);
    g_all = c.list;
    g_nall = c.n;
    g_next = g_done = 0;
    pthread_mutex_unlock(&g_lock);
    LOGI("covers: %d games to check%s", c.n, ok ? "" : " (RomM couldn't be reached for all of them)");
    return ok ? 0 : -1;
}

/* The next cover to fetch: the pages being looked at first, then the full
 * pass. "" when there's nothing to do. */
static void next_job(char *out, size_t n, int *from_pass) {
    out[0] = 0;
    *from_pass = 0;
    pthread_mutex_lock(&g_lock);
    if (g_nwarm) {
        str_copy(out, n, g_warm[0]);
        g_nwarm--;
        memmove(g_warm[0], g_warm[1], sizeof g_warm[0] * (size_t)g_nwarm);
    } else if (g_next < g_nall) {
        str_copy(out, n, g_all[g_next++]);
        *from_pass = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        if (!enabled() || busy()) {
            pthread_mutex_lock(&g_lock);
            str_copy(g_state, sizeof g_state, enabled() ? "paused" : "idle");
            pthread_mutex_unlock(&g_lock);
            sleep(2);
            continue;
        }
        char p[512];
        int from_pass;
        next_job(p, sizeof p, &from_pass);
        if (!p[0]) {
            pthread_mutex_lock(&g_lock);
            if (g_next >= g_nall && g_nall) str_copy(g_state, sizeof g_state, "done");
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 30;
            pthread_cond_timedwait(&g_wake, &g_lock, &until);
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        if (from_pass) {
            pthread_mutex_lock(&g_lock);
            str_copy(g_state, sizeof g_state, "running");
            long long used = g_cached_bytes;
            pthread_mutex_unlock(&g_lock);
            /* Stop short of the limit, so pruning never undoes the pass. */
            if (used >= 0 && used > cache_limit() * 9 / 10) {
                pthread_mutex_lock(&g_lock);
                g_next = g_nall;
                str_copy(g_state, sizeof g_state, "full");
                pthread_mutex_unlock(&g_lock);
                LOGW("covers: the cache is nearly full, the rest wait");
                continue;
            }
        }
        if (have(p)) {
            /* Still a current cover: keep it among the newest, so pruning takes
             * replaced and deleted covers first. */
            char cached[PATH_MAX_LEN];
            cache_path(p, "", cached, sizeof cached);
            if (from_pass) utime(cached, NULL);
        } else {
            char *body = NULL;
            size_t len = 0;
            const char *type;
            if (covers_fetch(p, &body, &len, &type) == 0) free(body);
        }
        if (from_pass) {
            pthread_mutex_lock(&g_lock);
            g_done++;
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}

/* Starts a full pass after setup, then once a day (or when asked). */
static void *planner(void *arg) {
    (void)arg;
    cache_scan(0);
    /* How often it looks for work; ROMM_SYNC_COVERS_TICK shortens it in tests. */
    const char *env = getenv("ROMM_SYNC_COVERS_TICK");
    unsigned tick = env && atoi(env) > 0 ? (unsigned)atoi(env) : 20;
    time_t retry_at = 0; /* after a pass RomM couldn't list in full */
    for (;;) {
        sleep(tick);
        pthread_mutex_lock(&g_lock);
        int prune = g_prune;
        g_prune = 0;
        pthread_mutex_unlock(&g_lock);
        if (prune) cache_scan(1);
        if (!enabled() || busy()) continue;
        pthread_mutex_lock(&g_lock);
        int running = g_next < g_nall, asked = g_refresh;
        g_refresh = 0;
        pthread_mutex_unlock(&g_lock);
        if (running) continue;
        state_lock();
        double last = jget_num(state_section("covers"), "last_full", 0);
        state_unlock();
        if (!asked && last > 0 && time(NULL) - (time_t)last < FULL_EVERY_SEC) continue;
        if (!asked && time(NULL) < retry_at) continue;
        /* A pass RomM couldn't list in full isn't counted: it's tried again in
         * a few minutes, not a day. */
        if (list_all() == 0) {
            state_lock();
            jset_num(state_section("covers"), "last_full", (double)time(NULL));
            state_save();
            state_unlock();
        } else {
            retry_at = time(NULL) + 300;
        }
        pthread_mutex_lock(&g_lock);
        pthread_cond_broadcast(&g_wake);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void covers_init(void) {
    thread_start(planner, NULL);
    for (int i = 0; i < workers(); i++) thread_start(worker, NULL);
}

cJSON *covers_status(void) {
    cJSON *j = cJSON_CreateObject();
    config_lock();
    cJSON_AddBoolToObject(j, "enabled", g_cfg.cover_cache);
    config_unlock();
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(j, "state", g_state);
    cJSON_AddNumberToObject(j, "done", g_done);
    cJSON_AddNumberToObject(j, "total", g_nall);
    cJSON_AddNumberToObject(j, "cached_mb", g_cached_bytes > 0 ? (double)(g_cached_bytes / (1024 * 1024)) : 0);
    pthread_mutex_unlock(&g_lock);
    cJSON_AddNumberToObject(j, "limit_mb", (double)(cache_limit() / (1024 * 1024)));
    return j;
}
