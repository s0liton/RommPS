/* Polls the save and state folders once a second and asks for a sync when a
 * changed file has been left alone for SETTLE_SEC. Files that changed before
 * startup are left to the startup sync. */
#include "watch.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "memcard.h"
#include "profiles.h"
#include "state.h"
#include "sync.h"
#include "util.h"

#define POLL_SEC        1
#define SETTLE_SEC      2
#define RELOAD_DIRS_SEC 60
#define MAX_DEPTH       3

typedef struct {
    char path[PATH_MAX_LEN];
    int64_t mtime; /* nanoseconds */
    off_t size;
    int64_t synced_mtime; /* last version handed to a sync */
    off_t synced_size;
    time_t changed_at;   /* when an unsynced change was seen, 0 if none */
    int seen;
} watched;

typedef struct {
    char dir[PATH_MAX_LEN];
    char exts[256];
} watch_dir;

static watched *g_files;
static int g_nfiles, g_capfiles;

static watched *find_file(const char *path) {
    for (int i = 0; i < g_nfiles; i++)
        if (!strcmp(g_files[i].path, path)) return &g_files[i];
    return NULL;
}

static int load_dirs(watch_dir **out) {
    profile_map *maps = NULL;
    config_lock();
    int n = profiles_expand(g_cfg.profiles, &maps);
    int states = g_cfg.sync_states;
    config_unlock();

    int cnt = 0, cap = 0;
    watch_dir *dirs = NULL;
    for (int i = 0; i < n; i++) {
        const profile_map *m = &maps[i];
        if (!m->sync_saves) continue;
        for (int k = 0; k < (states ? 2 : 1); k++) {
            const char *d = m->in_content[k] ? m->rom_dir : (k ? m->state_dir : m->save_dir);
            const char *e = k ? m->state_exts : m->save_exts;
            if (!d[0]) continue;
            watch_dir *w = NULL;
            for (int j = 0; j < cnt; j++)
                if (!strcmp(dirs[j].dir, d)) w = &dirs[j];
            if (!w) {
                if (cnt == cap) {
                    cap = cap ? cap * 2 : 8;
                    dirs = realloc(dirs, sizeof *dirs * (size_t)cap);
                }
                w = &dirs[cnt++];
                str_copy(w->dir, sizeof w->dir, d);
                w->exts[0] = 0;
            }
            if (!strstr(w->exts, e)) {
                if (w->exts[0]) strncat(w->exts, ",", sizeof w->exts - strlen(w->exts) - 1);
                strncat(w->exts, e, sizeof w->exts - strlen(w->exts) - 1);
            }
        }
    }
    /* Folders only reached through per-game overrides. */
    char all_exts[256] = "";
    for (int i = 0; i < n; i++) {
        const char *e[2] = {maps[i].save_exts, maps[i].state_exts};
        for (int k = 0; k < 2; k++) {
            if (strstr(all_exts, e[k])) continue;
            if (all_exts[0]) strncat(all_exts, ",", sizeof all_exts - strlen(all_exts) - 1);
            strncat(all_exts, e[k], sizeof all_exts - strlen(all_exts) - 1);
        }
    }
    free(maps);
    static char scanned[64][1024];
    static int any_file[64];
    int ns = sync_scanned_dirs(scanned, any_file, 64);
    for (int i = 0; i < ns; i++) {
        int dup = 0;
        for (int j = 0; j < cnt; j++)
            if (!strcmp(dirs[j].dir, scanned[i])) dup = 1;
        if (dup) continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 8;
            dirs = realloc(dirs, sizeof *dirs * (size_t)cap);
        }
        str_copy(dirs[cnt].dir, sizeof dirs[cnt].dir, scanned[i]);
        str_copy(dirs[cnt].exts, sizeof dirs[cnt].exts, any_file[i] ? "*" : all_exts);
        cnt++;
    }

    char cards[2][1024];
    int nc = memcard_dirs(cards, 2);
    for (int i = 0; i < nc; i++) {
        if (cnt == cap) {
            cap = cap ? cap * 2 : 8;
            dirs = realloc(dirs, sizeof *dirs * (size_t)cap);
        }
        str_copy(dirs[cnt].dir, sizeof dirs[cnt].dir, cards[i]);
        str_copy(dirs[cnt].exts, sizeof dirs[cnt].exts, ".ps2");
        cnt++;
    }
    *out = dirs;
    return cnt;
}

static void scan(const char *dir, const char *exts, int depth, int baseline, time_t now) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        int64_t mt = file_mtime_ns(path);
        if (S_ISDIR(st.st_mode)) {
            if (depth < MAX_DEPTH) scan(path, exts, depth + 1, baseline, now);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !ext_in_list(de->d_name, exts)) continue;

        watched *w = find_file(path);
        if (!w) {
            if (g_nfiles == g_capfiles) {
                g_capfiles = g_capfiles ? g_capfiles * 2 : 128;
                g_files = realloc(g_files, sizeof *g_files * (size_t)g_capfiles);
            }
            w = &g_files[g_nfiles++];
            memset(w, 0, sizeof *w);
            str_copy(w->path, sizeof w->path, path);
            if (baseline) {
                w->synced_mtime = mt;
                w->synced_size = st.st_size;
            } else {
                w->changed_at = now;
            }
        } else if (w->mtime != mt || w->size != st.st_size) {
            w->changed_at = now; /* still being written */
        }
        w->mtime = mt;
        w->size = st.st_size;
        w->seen = 1;
    }
    closedir(d);
}

/* The file is what the last transfer left, e.g. a save we just downloaded. */
static int written_by_sync(const watched *w) {
    int same = 0;
    state_lock();
    const cJSON *e = state_entry("saves", w->path);
    if (!e) e = state_entry("states", w->path);
    char ns[24];
    snprintf(ns, sizeof ns, "%lld", (long long)w->mtime);
    if (e && !strcmp(jget_str(e, "mtime_ns", ""), ns) && (off_t)jget_num(e, "size", -1) == w->size) same = 1;
    state_unlock();
    return same;
}

static void *watch_thread(void *arg) {
    (void)arg;
    watch_dir *dirs = NULL;
    int ndirs = 0, baseline = 1;
    time_t dirs_loaded = 0;

    for (;;) {
        time_t now = time(NULL);
        int enabled;
        config_lock();
        enabled = g_cfg.sync_on_change && g_cfg.setup_complete;
        config_unlock();
        if (!config_is_paired() || !enabled) {
            baseline = 1;
            g_nfiles = 0;
            sleep(POLL_SEC);
            continue;
        }
        if (!dirs || now - dirs_loaded >= RELOAD_DIRS_SEC) {
            free(dirs);
            ndirs = load_dirs(&dirs);
            dirs_loaded = now;
        }

        for (int i = 0; i < g_nfiles; i++) g_files[i].seen = 0;
        for (int i = 0; i < ndirs; i++) scan(dirs[i].dir, dirs[i].exts, 0, baseline, now);
        baseline = 0;

        for (int i = 0; i < g_nfiles;) {
            if (!g_files[i].seen) g_files[i] = g_files[--g_nfiles];
            else i++;
        }

        int ready = 0;
        const char *first = NULL;
        for (int i = 0; i < g_nfiles; i++) {
            watched *w = &g_files[i];
            if (!w->changed_at || now - w->changed_at < SETTLE_SEC) continue;
            w->changed_at = 0;
            if (w->mtime == w->synced_mtime && w->size == w->synced_size) continue;
            w->synced_mtime = w->mtime;
            w->synced_size = w->size;
            if (written_by_sync(w)) continue;
            if (!first) first = w->path;
            ready++;
        }
        if (ready) {
            LOGI("save changed on disk: %s%s", path_basename(first), ready > 1 ? " (+ more)" : "");
            sync_request("save changed");
        }
        sleep(POLL_SEC);
    }
    return NULL;
}

void watch_init(void) {
    thread_start(watch_thread, NULL);
}
