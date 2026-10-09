#include "sync.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "bundle.h"
#include "gameid.h"
#include "http.h"
#include "memcard.h"
#include "platform.h"
#include "profiles.h"
#include "romm.h"
#include "state.h"
#include "util.h"
#include "zip.h"

/* Never treated as games. */
static const char *IGNORED_EXTS = ".part,.tmp,.png,.jpg,.jpeg,.txt,.nfo,.xml,.json,.cfg,.opt,.srm,.state*,.sav";

typedef struct {
    char path[PATH_MAX_LEN];
    int rom_id;
    const profile_map *map;
    char stems[8][256];
    int nstems;
    char md5[33];
    /* For profiles that name files by them (save_stems, state_stems): */
    char serial[24];      /* a PS1 disc's, "SCUS-94194" */
    char fnv64[17];       /* FNV-1a 64 of the game file */
    char safe_stem[256];  /* stems[0], non-alphanumerics as "_" */
    rom_rules rules;
} local_rom;

typedef struct {
    char path[PATH_MAX_LEN];
    int rom_id;
    char slot[48];
    char hash[33];
    time_t mtime;
    int64_t mtime_ns;
    int64_t size;
    const profile_map *map;
    const local_rom *rom;
    int is_bundle;          /* several files synced as one zip, see bundle.h */
    char game_id[16];
    char upload_name[256];
} local_file;

typedef struct {
    profile_map *maps;
    int nmaps;
    local_rom *roms;
    int nroms, caproms;
    local_file *saves;
    int nsaves, capsaves;
    local_file *states;
    int nstates, capstates;
    cJSON *platforms;
    char slot[32], device_id[64];
    int keep_backups;
    int server_versions;
    conflict_policy policy;
} scan_ctx;

static pthread_mutex_t g_run_lock = PTHREAD_MUTEX_INITIALIZER; /* one sync at a time */
static pthread_mutex_t g_req_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_req_cond = PTHREAD_COND_INITIALIZER;
static char g_req_reason[64];
static volatile int g_running;
static volatile time_t g_last_run;
static char g_phase[128] = "idle";
static cJSON *g_play_sessions; /* guarded by g_req_lock */
static cJSON *g_unmatched;     /* guarded by state_lock */
static cJSON *g_strays;        /* guarded by state_lock */
static cJSON *g_roots;         /* folders scanned by the last sync, guarded by state_lock */
static cJSON *g_bundle_roots;  /* bundle folders, watched for any file, guarded by state_lock */
static volatile int g_game_running;
static volatile int g_sync_count;

static void set_phase(const char *p) { str_copy(g_phase, sizeof g_phase, p); }


static char *url_q(const char *s) { return http_escape(s ? s : ""); }

static const char *slot_for(const scan_ctx *c, const profile_map *m, const char *file,
                            char *buf, size_t n) {
    /* The main save uses the shared slot so other RomM clients pair with it.
     * Extra files such as .rtc get a slot of their own. */
    char first[32];
    str_copy(first, sizeof first, m->save_exts);
    char *comma = strchr(first, ',');
    if (comma) *comma = 0;
    const char *ext = path_ext(file);
    char base[64];
    /* An extra emulator whose saves can't be shared keeps them apart. */
    if (m->own_slot) snprintf(base, sizeof base, "%s-%s", c->slot, m->profile_id);
    else str_copy(base, sizeof base, c->slot);
    if (!first[0] || str_ieq(ext, first)) {
        str_copy(buf, n, base);
    } else {
        snprintf(buf, n, "%s-%s", base, ext[0] == '.' ? ext + 1 : ext);
    }
    return buf;
}

static int ignored_rom_file(const char *name, const profile_map *m) {
    if (name[0] == '.') return 1;
    if (ext_in_list(name, IGNORED_EXTS)) return 1;
    if (ext_in_list(name, m->save_exts) || ext_in_list(name, m->state_exts)) return 1;
    return 0;
}

static void add_stem(local_rom *r, const char *stem) {
    if (!stem[0] || r->nstems >= 8) return;
    for (int i = 0; i < r->nstems; i++)
        if (!strcmp(r->stems[i], stem)) return;
    str_copy(r->stems[r->nstems++], sizeof r->stems[0], stem);
}


/* The disc or file of a game that a serial or hash is read from: the game
 * itself, or for a folder its first .cue, .chd, .iso or .bin. */
static int game_file(const local_rom *r, char *out, size_t n) {
    if (!dir_exists(r->path)) {
        str_copy(out, n, r->path);
        return 0;
    }
    static const char *const order[] = {".cue", ".chd", ".iso", ".bin"};
    for (int k = 0; k < 4; k++) {
        DIR *d = opendir(r->path);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] != '.' && str_ends_with_ci(de->d_name, order[k])) {
                path_join(out, n, r->path, de->d_name);
                closedir(d);
                return 0;
            }
        }
        if (d) closedir(d);
    }
    return -1;
}

static int map_uses(const profile_map *m, const char *var) {
    return strstr(m->save_stems, var) || strstr(m->state_stems, var) || strstr(m->save_name, var) ||
           strstr(m->state_name, var);
}

/* The template variables a game's file names can use. */
static void name_vars(const local_rom *r, const char *ext, const char *vars[13]) {
    const char *v[] = {"rom_stem", r->stems[0], "ext", ext ? ext : "", "rom_md5", r->md5, "serial", r->serial,
                       "rom_fnv64", r->fnv64, "rom_stem_safe", r->safe_stem, NULL};
    memcpy(vars, v, sizeof v);
}

/* Works out what the profile's extra names need (cached per file in the
 * state's roms entry, keyed by size and mtime), then adds those names. */
static void add_derived_stems(local_rom *r, cJSON *e) {
    const profile_map *m = r->map;
    size_t k = 0;
    for (const char *p = r->stems[0]; *p && k + 1 < sizeof r->safe_stem; p++)
        r->safe_stem[k++] = isalnum((unsigned char)*p) ? *p : '_';
    r->safe_stem[k] = 0;
    char file[PATH_MAX_LEN];
    int need_serial = map_uses(m, "{serial}"), need_fnv = map_uses(m, "{rom_fnv64}");
    if ((need_serial || need_fnv) && game_file(r, file, sizeof file) == 0) {
        double size = (double)file_size(file), mtime = (double)file_mtime(file);
        int fresh = e && jget_num(e, "id_size", -1) == size && jget_num(e, "id_mtime", -1) == mtime;
        if (need_serial) {
            if (fresh && cJSON_GetObjectItemCaseSensitive(e, "serial"))
                str_copy(r->serial, sizeof r->serial, jget_str(e, "serial", ""));
            else if (gameid_psx(file, r->serial, sizeof r->serial) != 0)
                r->serial[0] = 0;
        }
        if (need_fnv) {
            if (fresh && cJSON_GetObjectItemCaseSensitive(e, "fnv64"))
                str_copy(r->fnv64, sizeof r->fnv64, jget_str(e, "fnv64", ""));
            else if (fnv1a64_file_hex(file, r->fnv64) != 0)
                r->fnv64[0] = 0;
        }
        if (e) {
            jset_num(e, "id_size", size);
            jset_num(e, "id_mtime", mtime);
            if (need_serial) jset_str(e, "serial", r->serial);
            if (need_fnv) jset_str(e, "fnv64", r->fnv64);
        }
    }
    const char *vars[13];
    name_vars(r, "", vars);
    for (int pass = 0; pass < 2; pass++) {
        char list[256], *save = NULL;
        str_copy(list, sizeof list, pass ? m->state_stems : m->save_stems);
        for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            char stem[256];
            str_template(stem, sizeof stem, t, vars);
            /* A template whose value is unknown (no serial) names nothing. */
            if (stem[0] && !strchr(stem, '{') && strcmp(stem, "_1") != 0 && stem[0] != '_') add_stem(r, stem);
        }
    }
}

static int platform_ids_for_map(const scan_ctx *c, const profile_map *m, int *ids, int max) {
    int n = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, c->platforms) {
        if (n >= max) break;
        if (profile_map_handles(m, jget_str(p, "slug", NULL), jget_str(p, "fs_slug", NULL)))
            ids[n++] = (int)jget_num(p, "id", 0);
    }
    return n;
}

struct server_rom_match {
    const char *name, *stem;
    cJSON *exact, *by_stem; /* copies of the matching games */
};

/* An exact file name match wins; otherwise the first game with the same stem. */
static int match_server_rom(const cJSON *it, const cJSON *page, int first, void *ctx) {
    struct server_rom_match *m = ctx;
    if (!it) return 0;
    if (str_ieq(jget_str(it, "fs_name", ""), m->name)) {
        m->exact = cJSON_Duplicate(it, 1);
        return 1;
    }
    if (!m->by_stem && str_ieq(jget_str(it, "fs_name_no_ext", ""), m->stem)) m->by_stem = cJSON_Duplicate(it, 1);
    return 0;
}

/* The RomM game whose file name matches a local file or folder. */
static cJSON *resolve_rom_on_server(const scan_ctx *c, const profile_map *m, const char *name,
                                    int is_dir) {
    int ids[16];
    int nids = platform_ids_for_map(c, m, ids, 16);
    if (nids == 0) return NULL;
    char stem[256], path[2048];
    if (is_dir)
        str_copy(stem, sizeof stem, name);
    else
        path_stem(stem, sizeof stem, name);
    char *q = url_q(stem);
    int o = snprintf(path, sizeof path, "/api/roms?search_term=%s&with_total=false&with_char_index=false" ROMS_LEAN, q);
    free(q);
    for (int i = 0; i < nids; i++) o += snprintf(path + o, sizeof path - (size_t)o, "&platform_ids=%d", ids[i]);

    struct server_rom_match match = {name, stem, NULL, NULL};
    long st = 0;
    romm_list(path, 0, 50, match_server_rom, &match, &st);
    if (match.exact) {
        cJSON_Delete(match.by_stem);
        return match.exact;
    }
    return match.by_stem;
}

static void record_local_rom(const char *path, int rom_id, const profile_map *m, const cJSON *srv,
                             const char *md5, const local_rom *r) {
    const char *name = jget_str(srv, "name", ""), *fs_name = jget_str(srv, "fs_name", "");
    cJSON *e = state_entry_ensure("roms", path);
    jset_num(e, "rom_id", rom_id);
    /* For the app's "on this console" row: the cover and the platform. */
    jset_num(e, "platform_id", jget_num(srv, "platform_id", 0));
    jset_str(e, "cover", jget_str(srv, "path_cover_small", ""));
    jset_str(e, "profile", m->profile_id);
    jset_str(e, "platform_dir", m->platform_dir);
    jset_str(e, "name", name);
    jset_str(e, "fs_name", fs_name);
    if (md5 && md5[0]) jset_str(e, "md5", md5);
    cJSON *stems = cJSON_CreateArray();
    for (int i = 0; i < r->nstems; i++) cJSON_AddItemToArray(stems, cJSON_CreateString(r->stems[i]));
    if (cJSON_GetObjectItemCaseSensitive(e, "stems"))
        cJSON_ReplaceItemInObjectCaseSensitive(e, "stems", stems);
    else
        cJSON_AddItemToObject(e, "stems", stems);
    cJSON_DeleteItemFromObjectCaseSensitive(e, "miss_at");
}

static void scan_rom_dir(scan_ctx *c, const profile_map *m) {
    DIR *d = opendir(m->rom_dir);
    if (!d) return;
    struct dirent *de;
    time_t now = time(NULL);
    while ((de = readdir(d))) {
        if (ignored_rom_file(de->d_name, m)) continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, m->rom_dir, de->d_name);
        int is_dir = dir_exists(path);
        if (!is_dir && !file_exists(path)) continue;

        /* Several platforms can share one folder. */
        int dup = 0;
        for (int i = 0; i < c->nroms; i++)
            if (!strcmp(c->roms[i].path, path)) dup = 1;
        if (dup) continue;

        local_rom r;
        memset(&r, 0, sizeof r);
        str_copy(r.path, sizeof r.path, path);
        r.map = m;
        char stem[256];
        if (is_dir) {
            add_stem(&r, de->d_name);
            DIR *sd = opendir(path);
            struct dirent *se;
            while (sd && (se = readdir(sd))) {
                if (se->d_name[0] == '.') continue;
                path_stem(stem, sizeof stem, se->d_name);
                if (ext_in_list(se->d_name, ".cue,.m3u,.gdi,.chd,.ccd,.iso,.pbp")) add_stem(&r, stem);
            }
            if (sd) closedir(sd);
        } else {
            path_stem(stem, sizeof stem, de->d_name);
            add_stem(&r, stem);
        }

        state_lock();
        cJSON *e = state_entry("roms", path);
        int rom_id = e ? (int)jget_num(e, "rom_id", 0) : 0;
        time_t miss_at = e ? (time_t)jget_num(e, "miss_at", 0) : 0;
        if (e) {
            const cJSON *s;
            cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(e, "stems"))
                if (cJSON_IsString(s)) add_stem(&r, s->valuestring);
            str_copy(r.md5, sizeof r.md5, jget_str(e, "md5", ""));
        }
        state_unlock();

        /* Ask RomM about new files. Misses are retried after a day. */
        if (!rom_id && (!miss_at || now - miss_at > 86400)) {
            cJSON *srv = resolve_rom_on_server(c, m, de->d_name, is_dir);
            state_lock();
            if (srv) {
                rom_id = (int)jget_num(srv, "id", 0);
                add_stem(&r, jget_str(srv, "fs_name_no_ext", ""));
                str_copy(r.md5, sizeof r.md5, jget_str(srv, "md5_hash", ""));
                record_local_rom(path, rom_id, m, srv, r.md5, &r);
                LOGI("matched %s -> RomM rom %d", path, rom_id);
            } else {
                cJSON *ne = state_entry_ensure("roms", path);
                jset_num(ne, "rom_id", 0);
                jset_num(ne, "miss_at", (double)now);
                jset_str(ne, "profile", m->profile_id);
            }
            state_unlock();
            cJSON_Delete(srv);
        }
        if (!rom_id) {
            state_lock();
            cJSON_AddItemToArray(g_unmatched, cJSON_CreateString(path));
            state_unlock();
            continue;
        }
        r.rom_id = rom_id;
        if (m->save_stems[0] || m->state_stems[0] || map_uses(m, "{rom_stem_safe}")) {
            state_lock();
            add_derived_stems(&r, state_entry("roms", path));
            state_save();
            state_unlock();
        }
        profile_rules_for(m, path, &r.rules);
        if (c->nroms == c->caproms) {
            c->caproms = c->caproms ? c->caproms * 2 : 64;
            c->roms = realloc(c->roms, sizeof *c->roms * (size_t)c->caproms);
        }
        c->roms[c->nroms++] = r;
    }
    closedir(d);
}


/* The folder the emulator reads this game's saves (or states) from. Returns 0
 * when saves are sorted by core and the core name isn't known. */
static int expected_dir(const local_rom *r, int states, char *out, size_t n) {
    const rom_rules *ru = &r->rules;
    char content_dir[PATH_MAX_LEN];
    str_copy(content_dir, sizeof content_dir, r->path);
    if (!dir_exists(r->path)) { /* multi-file games are folders themselves */
        char *slash = strrchr(content_dir, '/');
        if (slash) *slash = 0;
    }
    if (ru->in_content[states]) {
        str_copy(out, n, content_dir);
        return 1;
    }
    char tmp[PATH_MAX_LEN];
    str_copy(out, n, states ? ru->state_dir : ru->save_dir);
    if (ru->by_content[states]) {
        path_join(tmp, sizeof tmp, out, path_basename(content_dir));
        str_copy(out, n, tmp);
    }
    if (ru->by_core[states]) {
        if (!r->map->core_name[0]) return 0;
        path_join(tmp, sizeof tmp, out, r->map->core_name);
        str_copy(out, n, tmp);
    }
    return 1;
}

/* The game a save belongs to, by name: "Game" owns "Game.srm", "Game.1a2b.sav"
 * and "Game.state3". *active is 0 when the file isn't where the emulator reads it. */
static const local_rom *match_rom(const scan_ctx *c, const char *file_dir, const char *file_name,
                                  int states, int *active) {
    const local_rom *best = NULL, *best_active = NULL;
    size_t best_len = 0, best_active_len = 0;
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (!r->rules.sync_saves || (!states && r->map->save_layout[0])) continue;
        if (!ext_in_list(file_name, states ? r->map->state_exts : r->map->save_exts)) continue;
        for (int s = 0; s < r->nstems; s++) {
            size_t l = strlen(r->stems[s]);
            if (strncmp(file_name, r->stems[s], l) != 0 || file_name[l] != '.') continue;
            char exp[PATH_MAX_LEN];
            int known = expected_dir(r, states, exp, sizeof exp);
            /* Unknown core folder: accept it anywhere below the base folder. */
            int here = known ? !strcmp(exp, file_dir) : 1;
            if (here && l > best_active_len) {
                best_active = r;
                best_active_len = l;
            }
            if (l > best_len) {
                best = r;
                best_len = l;
            }
        }
    }
    *active = best_active != NULL;
    return best_active ? best_active : best;
}

static void add_file(local_file **arr, int *n, int *cap, const local_file *f) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *arr = realloc(*arr, sizeof **arr * (size_t)*cap);
    }
    (*arr)[(*n)++] = *f;
}

/* Reuse the recorded hash only if the file is untouched, down to the
 * nanosecond. Seconds weren't good enough: SRAM saves never change size, and
 * emulators are happy to rewrite them twice in the same second. */
static void hash_cached(const char *section, local_file *f) {
    char ns[24];
    snprintf(ns, sizeof ns, "%lld", (long long)f->mtime_ns);
    state_lock();
    cJSON *e = state_entry(section, f->path);
    if (e && !strcmp(jget_str(e, "mtime_ns", ""), ns) && (int64_t)jget_num(e, "size", -1) == f->size)
        str_copy(f->hash, sizeof f->hash, jget_str(e, "hash", ""));
    state_unlock();
    if (!f->hash[0]) md5_file_hex(f->path, f->hash);
}

static int already_seen(const scan_ctx *c, const char *path) {
    for (int i = 0; i < c->nsaves; i++)
        if (!strcmp(c->saves[i].path, path)) return 1;
    for (int i = 0; i < c->nstates; i++)
        if (!strcmp(c->states[i].path, path)) return 1;
    const cJSON *e;
    cJSON_ArrayForEach(e, g_strays)
        if (!strcmp(jget_str(e, "path", ""), path)) return 1;
    return 0;
}

static void add_stray(const local_rom *r, const char *path, int states) {
    char exp_dir[PATH_MAX_LEN], exp[PATH_MAX_LEN];
    expected_dir(r, states, exp_dir, sizeof exp_dir);
    path_join(exp, sizeof exp, exp_dir, path_basename(path));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "path", path);
    cJSON_AddStringToObject(o, "expected_path", exp);
    cJSON_AddBoolToObject(o, "expected_exists", file_exists(exp));
    cJSON_AddBoolToObject(o, "state", states);
    cJSON_AddNumberToObject(o, "rom_id", r->rom_id);
    cJSON_AddStringToObject(o, "emulator", r->map->profile_name);
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    cJSON_AddStringToObject(o, "rom_name", e ? jget_str(e, "name", "") : "");
    cJSON_AddItemToArray(g_strays, o);
    state_unlock();
}

static void scan_file_dir(scan_ctx *c, const char *dir, int depth, int states) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        if (dir_exists(path)) {
            /* Sorted layouts go two levels deep. */
            if (depth < 2) scan_file_dir(c, path, depth + 1, states);
            continue;
        }
        if (str_ends_with_ci(de->d_name, ".tmp") || str_ends_with_ci(de->d_name, ".part")) continue;
        if (already_seen(c, path)) continue;
        int active = 0;
        const local_rom *r = match_rom(c, dir, de->d_name, states, &active);
        if (!r) continue;
        if (!active) {
            /* Possibly an old copy. Report it and leave it alone. */
            add_stray(r, path, states);
            continue;
        }

        local_file f;
        memset(&f, 0, sizeof f);
        str_copy(f.path, sizeof f.path, path);
        f.rom_id = r->rom_id;
        f.map = r->map;
        f.rom = r;
        f.mtime = file_mtime(path);
        f.mtime_ns = file_mtime_ns(path);
        f.size = file_size(path);
        if (!states) {
            slot_for(c, r->map, de->d_name, f.slot, sizeof f.slot);
            /* One file per game and slot; the newest wins. */
            int replaced = 0;
            for (int i = 0; i < c->nsaves; i++) {
                local_file *o = &c->saves[i];
                if (o->rom_id == f.rom_id && !strcmp(o->slot, f.slot)) {
                    LOGW("two local saves for rom %d slot %s: %s, %s (using newest)", f.rom_id,
                         f.slot, o->path, f.path);
                    if (f.mtime > o->mtime) {
                        hash_cached("saves", &f);
                        *o = f;
                    }
                    replaced = 1;
                    break;
                }
            }
            if (replaced) continue;
            hash_cached("saves", &f);
            add_file(&c->saves, &c->nsaves, &c->capsaves, &f);
        } else {
            hash_cached("states", &f);
            add_file(&c->states, &c->nstates, &c->capstates, &f);
        }
    }
    closedir(d);
}

static void scan_bundles(scan_ctx *c);

/* Base folder a game's saves (or states) live under, before sorting. */
static void base_dir(const local_rom *r, int states, char *out, size_t n) {
    if (r->rules.in_content[states]) {
        str_copy(out, n, r->path);
        if (!dir_exists(r->path)) {
            char *slash = strrchr(out, '/');
            if (slash) *slash = 0;
        }
    } else {
        str_copy(out, n, states ? r->rules.state_dir : r->rules.save_dir);
    }
}

static void scan_saves(scan_ctx *c) {
    state_lock();
    cJSON_Delete(g_strays);
    g_strays = cJSON_CreateArray();
    cJSON_Delete(g_roots);
    g_roots = cJSON_CreateArray();
    cJSON_Delete(g_bundle_roots);
    g_bundle_roots = cJSON_CreateArray();
    state_unlock();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < c->nroms; i++) {
            if (!c->roms[i].rules.sync_saves) continue;
            char dir[PATH_MAX_LEN];
            base_dir(&c->roms[i], pass, dir, sizeof dir);
            int seen = 0;
            for (int k = 0; k < i && !seen; k++) {
                char other[PATH_MAX_LEN];
                if (!c->roms[k].rules.sync_saves) continue;
                base_dir(&c->roms[k], pass, other, sizeof other);
                seen = !strcmp(other, dir);
            }
            if (seen || !dir[0]) continue;
            scan_file_dir(c, dir, 0, pass);
            state_lock();
            cJSON_AddItemToArray(g_roots, cJSON_CreateString(dir));
            state_unlock();
        }
    }
    scan_bundles(c);
}


static int scan_prepare(scan_ctx *c) {
    memset(c, 0, sizeof *c);
    config_lock();
    c->nmaps = profiles_expand(g_cfg.profiles, &c->maps);
    str_copy(c->slot, sizeof c->slot, g_cfg.slot);
    str_copy(c->device_id, sizeof c->device_id, g_cfg.device_id);
    c->keep_backups = g_cfg.keep_backups;
    c->server_versions = g_cfg.server_versions;
    c->policy = g_cfg.conflicts;
    config_unlock();

    long st = 0;
    c->platforms = romm_call("GET", "/api/platforms", NULL, &st);
    if (st != 200 || !cJSON_IsArray(c->platforms)) {
        LOGE("could not list platforms (HTTP %ld)", st);
        return -1;
    }
    state_lock();
    cJSON_Delete(g_unmatched);
    g_unmatched = cJSON_CreateArray();
    state_unlock();
    for (int i = 0; i < c->nmaps; i++) scan_rom_dir(c, &c->maps[i]);
    return 0;
}

static void scan_free(scan_ctx *c) {
    free(c->maps);
    free(c->roms);
    free(c->saves);
    free(c->states);
    cJSON_Delete(c->platforms);
}

void sync_scan_local_roms(void) {
    if (!config_is_paired()) return;
    pthread_mutex_lock(&g_run_lock);
    scan_ctx c;
    if (scan_prepare(&c) == 0) {
        state_lock();
        state_save();
        state_unlock();
    }
    scan_free(&c);
    pthread_mutex_unlock(&g_run_lock);
}


static void backup_file(const scan_ctx *c, const char *path, int rom_id) {
    if (c->keep_backups <= 0 || !file_exists(path)) return;
    char dir[PATH_MAX_LEN], dst[PATH_MAX_LEN], sub[32];
    snprintf(sub, sizeof sub, "backups/%d", rom_id);
    path_join(dir, sizeof dir, plat_data_dir(), sub);
    mkdir_p(dir);
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    snprintf(dst, sizeof dst, "%s/%s.%s", dir, path_basename(path), ts);
    if (copy_file(path, dst) == 0) LOGI("backed up %s -> %s", path, dst);

    /* Keep the newest keep_backups copies. */
    DIR *d = opendir(dir);
    if (!d) return;
    char names[64][256];
    int n = 0;
    struct dirent *de;
    size_t bl = strlen(path_basename(path));
    while ((de = readdir(d)) && n < 64)
        if (!strncmp(de->d_name, path_basename(path), bl) && de->d_name[bl] == '.')
            str_copy(names[n++], sizeof names[0], de->d_name);
    closedir(d);
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++)
            if (strcmp(names[k], names[i]) < 0) {
                char t[256];
                memcpy(t, names[i], sizeof t);
                memcpy(names[i], names[k], sizeof t);
                memcpy(names[k], t, sizeof t);
            }
    for (int i = 0; i < n - c->keep_backups; i++) {
        char p[PATH_MAX_LEN];
        path_join(p, sizeof p, dir, names[i]);
        unlink(p);
    }
}

static void record_save(const local_file *f, int save_id, const char *hash) {
    state_lock();
    cJSON *e = state_entry_ensure("saves", f->path);
    jset_num(e, "rom_id", f->rom_id);
    jset_str(e, "slot", f->slot);
    jset_str(e, "hash", hash ? hash : f->hash);
    jset_num(e, "mtime", (double)(f->is_bundle ? f->mtime : file_mtime(f->path)));
    jset_num(e, "size", (double)(f->is_bundle ? f->size : file_size(f->path)));
    if (!f->is_bundle) {
        char ns[24];
        snprintf(ns, sizeof ns, "%lld", (long long)file_mtime_ns(f->path));
        jset_str(e, "mtime_ns", ns);
    }
    if (save_id) jset_num(e, "save_id", save_id);
    jset_num(e, "synced_at", (double)time(NULL));
    state_entry_remove("conflicts", f->path);
    state_unlock();
}

static const local_file *find_save(const scan_ctx *c, int rom_id, const char *slot);

/* Candidate folders for a game's bundled saves. */
static int bundle_bases(const local_rom *r, char bases[][PATH_MAX_LEN], int max) {
    static const char *defaults[][3] = {
        /* {save_path} follows RetroArch's sorting. {save_root} is the bare save
         * folder, because Dolphin ignores sort-by-core entirely and drops
         * dolphin-emu/User right in savefiles/. Thanks, Dolphin. */
        {"psp", "{save_path}/PSP/SAVEDATA,{save_root}/PPSSPP/PSP/SAVEDATA,{bios}/PPSSPP/PSP/SAVEDATA", NULL},
        {"gci", "{save_root}/dolphin-emu/User/GC,{save_path}/User/GC,{save_path}/dolphin-emu/User/GC,{bios}/dolphin-emu/User/GC", NULL},
        {"wii", "{save_root}/dolphin-emu/User/Wii,{save_path}/User/Wii,{save_path}/dolphin-emu/User/Wii,{bios}/dolphin-emu/User/Wii", NULL},
    };
    const char *tpl = r->map->bundle_dirs[0] ? r->map->bundle_dirs : NULL;
    for (size_t i = 0; !tpl && i < sizeof defaults / sizeof defaults[0]; i++)
        if (!strcmp(defaults[i][0], r->map->save_layout)) tpl = defaults[i][1];
    if (!tpl) return 0;
    char save_path[PATH_MAX_LEN], list[2048], *save = NULL;
    if (!expected_dir(r, 0, save_path, sizeof save_path)) str_copy(save_path, sizeof save_path, r->rules.save_dir);
    const char *vars[] = {"save_path", save_path, "save_root", r->rules.save_dir, "bios", r->map->bios_dir, "root", r->map->root, NULL};
    str_template(list, sizeof list, tpl, vars);
    int n = 0;
    for (char *t = strtok_r(list, ",", &save); t && n < max; t = strtok_r(NULL, ",", &save))
        str_copy(bases[n++], PATH_MAX_LEN, t);
    return n;
}

static int bundle_for_rom(const local_rom *r, const char *game_id, bundle *b) {
    char bases[6][PATH_MAX_LEN];
    int n = bundle_bases(r, bases, 6);
    return n ? bundle_open(b, r->map->save_layout, game_id, bases, n) : -1;
}

static int bundle_reopen(const local_file *f, bundle *b) { return bundle_for_rom(f->rom, f->game_id, b); }

static int norm_eq(const char *a, const char *b) {
    char x[256], y[256];
    size_t i = 0, j = 0;
    for (; *a && i < sizeof x - 1; a++) if (isalnum((unsigned char)*a)) x[i++] = (char)tolower((unsigned char)*a);
    for (; *b && j < sizeof y - 1; b++) if (isalnum((unsigned char)*b)) y[j++] = (char)tolower((unsigned char)*b);
    x[i] = y[j] = 0;
    return i >= 4 && j >= 4 && (strstr(x, y) || strstr(y, x));
}

/* PSP saves name their game in PARAM.SFO; used when the disc can't be read. */
static int psp_id_from_saves(const local_rom *r, const char *name, char *out, size_t n) {
    char bases[6][PATH_MAX_LEN];
    int nb = bundle_bases(r, bases, 6);
    for (int i = 0; i < nb; i++) {
        DIR *d = opendir(bases[i]);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            char sfo[PATH_MAX_LEN], title[256];
            snprintf(sfo, sizeof sfo, "%s/%s/PARAM.SFO", bases[i], de->d_name);
            if (strlen(de->d_name) < 9 || sfo_file_get(sfo, "TITLE", title, sizeof title) != 0) continue;
            if (!norm_eq(title, name)) continue;
            snprintf(out, n, "%.9s", de->d_name);
            closedir(d);
            return 0;
        }
        if (d) closedir(d);
    }
    return -1;
}

/* The game's id: from the disc image, RomM (title_id, 5.3+), or its PSP saves. */
static int rom_game_id(const local_rom *r, char *out, size_t n) {
    out[0] = 0;
    char name[256] = "";
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    if (e) {
        str_copy(out, n, jget_str(e, "game_id", ""));
        str_copy(name, sizeof name, jget_str(e, "name", ""));
    }
    state_unlock();
    if (out[0]) return 0;

    char disc[PATH_MAX_LEN];
    str_copy(disc, sizeof disc, r->path);
    if (dir_exists(r->path)) { /* multi-file game: use its first disc image */
        DIR *d = opendir(r->path);
        struct dirent *de;
        disc[0] = 0;
        while (d && (de = readdir(d)) && !disc[0])
            if (ext_in_list(de->d_name, ".iso,.cso,.rvz,.wia,.gcm,.ciso,.wbfs")) path_join(disc, sizeof disc, r->path, de->d_name);
        if (d) closedir(d);
    }
    int ok = disc[0] && (!strcmp(r->map->save_layout, "psp") ? gameid_psp(disc, out, n) : gameid_dolphin(disc, out, n)) == 0;
    if (!ok) {
        char api[64];
        long st = 0;
        snprintf(api, sizeof api, "/api/roms/%d", r->rom_id);
        cJSON *rom = romm_call("GET", api, NULL, &st);
        const char *tid = jget_str(rom, "title_id", "");
        size_t o = 0;
        for (; *tid && o + 1 < n; tid++) if (isalnum((unsigned char)*tid)) out[o++] = (char)toupper((unsigned char)*tid);
        out[o] = 0;
        ok = o >= 4;
        cJSON_Delete(rom);
    }
    if (!ok && !strcmp(r->map->save_layout, "psp")) {
        if (!name[0]) path_stem(name, sizeof name, r->path);
        ok = psp_id_from_saves(r, name, out, n) == 0;
    }
    if (!ok) {
        out[0] = 0;
        return -1;
    }
    state_lock();
    e = state_entry("roms", r->path);
    if (e) jset_str(e, "game_id", out);
    state_unlock();
    LOGI("game id for %s is %s", path_basename(r->path), out);
    return 0;
}

static void fill_bundle_file(local_file *f, const scan_ctx *c, const local_rom *r, const bundle *b) {
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s#%s", b->dir, b->game_id);
    f->rom_id = r->rom_id;
    f->map = r->map;
    f->rom = r;
    f->is_bundle = 1;
    str_copy(f->game_id, sizeof f->game_id, b->game_id);
    str_copy(f->slot, sizeof f->slot, c->slot);
    f->mtime = b->mtime;
    f->size = b->size;
    bundle_hash(b, f->hash);
    if (!strcmp(r->map->save_layout, "psp")) {
        snprintf(f->upload_name, sizeof f->upload_name, "%s.zip", b->game_id);
    } else {
        char stem[200];
        path_stem(stem, sizeof stem, r->path);
        snprintf(f->upload_name, sizeof f->upload_name, "%s.zip", stem);
    }
}

static void scan_bundles(scan_ctx *c) {
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (!r->rules.sync_saves || !bundle_supported(r->map->save_layout)) continue;
        char id[16];
        bundle b;
        if (rom_game_id(r, id, sizeof id) != 0 || bundle_for_rom(r, id, &b) != 0) continue;
        state_lock();
        cJSON_AddItemToArray(g_bundle_roots, cJSON_CreateString(b.dir));
        state_unlock();
        if (b.n > 0 && !find_save(c, r->rom_id, c->slot)) {
            local_file f;
            fill_bundle_file(&f, c, r, &b);
            add_file(&c->saves, &c->nsaves, &c->capsaves, &f);
        }
        bundle_free(&b);
    }
}

/* Downloads a save zip and unpacks it over the game's current files. With
 * record set, the result counts as synced; a manual restore leaves it to upload. */
static int do_download_bundle(const scan_ctx *c, int save_id, const local_rom *r, const char *slot, int session_id,
                              const char *server_hash, int record) {
    char id[16], api[256], zip[PATH_MAX_LEN], tmpdir[PATH_MAX_LEN];
    bundle b;
    if (rom_game_id(r, id, sizeof id) != 0 || bundle_for_rom(r, id, &b) != 0) {
        LOGE("no game id for %s, cannot place its save", path_basename(r->path));
        return -1;
    }
    char *dev = url_q(c->device_id);
    if (record) {
        /* optimistic=false: RomM counts the save as on this console once
         * /downloaded confirms it's written, not as soon as it's served. */
        int o = snprintf(api, sizeof api, "/api/saves/%d/content?device_id=%s&optimistic=false", save_id, dev);
        if (session_id) snprintf(api + o, sizeof api - (size_t)o, "&session_id=%d", session_id);
    } else {
        snprintf(api, sizeof api, "/api/saves/%d/content", save_id);
    }
    free(dev);
    snprintf(zip, sizeof zip, "%s/tmp/save-%d.zip", plat_data_dir(), save_id);
    snprintf(tmpdir, sizeof tmpdir, "%s/tmp/unpack-%d", plat_data_dir(), save_id);
    http_resp resp;
    int rc = -1;
    if (romm_download(api, zip, NULL, NULL, &resp) != 0) {
        LOGE("download of save %d failed: %s", save_id, resp.error);
        goto out;
    }
    if (b.n > 0) { /* keep a copy of what's there now */
        char cur[PATH_MAX_LEN];
        snprintf(cur, sizeof cur, "%s/tmp/%s.zip", plat_data_dir(), id);
        if (bundle_zip(&b, cur) == 0) backup_file(c, cur, r->rom_id);
        unlink(cur);
    }
    if (bundle_replace(&b, zip, tmpdir) != 0) {
        LOGE("save %d is not a usable zip", save_id);
        goto out;
    }
    rc = 0;
    LOGI("downloaded save %d into %s", save_id, b.dir);
    if (record) {
        local_file f;
        fill_bundle_file(&f, c, r, &b);
        str_copy(f.slot, sizeof f.slot, slot);
        record_save(&f, save_id, f.hash);
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "device_id", c->device_id);
        cJSON_AddStringToObject(body, "content_hash", server_hash && server_hash[0] ? server_hash : f.hash);
        snprintf(api, sizeof api, "/api/saves/%d/downloaded", save_id);
        long st = 0;
        cJSON_Delete(romm_call("POST", api, body, &st));
        if (st != 200) LOGW("RomM didn't take the confirmation for save %d (HTTP %ld); it may come down again", save_id, st);
        cJSON_Delete(body);
    }
out:
    unlink(zip);
    bundle_free(&b);
    return rc;
}

static int do_upload(const scan_ctx *c, const local_file *f, int session_id, int overwrite,
                     long *status_out) {
    char path[1024];
    char *slot = url_q(f->slot), *emu = url_q(f->map->emulator), *dev = url_q(c->device_id);
    int o = snprintf(path, sizeof path, "/api/saves?rom_id=%d&slot=%s&emulator=%s&device_id=%s&overwrite=%s",
                     f->rom_id, slot, emu, dev, overwrite ? "true" : "false");
    if (session_id) o += snprintf(path + o, sizeof path - (size_t)o, "&session_id=%d", session_id);
    if (c->server_versions > 0)
        snprintf(path + o, sizeof path - (size_t)o, "&autocleanup=true&autocleanup_limit=%d", c->server_versions);
    free(slot);
    free(emu);
    free(dev);
    long st = 0;
    cJSON *j;
    if (f->is_bundle) {
        char zip[PATH_MAX_LEN];
        bundle b;
        snprintf(zip, sizeof zip, "%s/tmp/upload-%d.zip", plat_data_dir(), f->rom_id);
        if (bundle_reopen(f, &b) != 0 || bundle_zip(&b, zip) != 0) {
            bundle_free(&b);
            if (status_out) *status_out = 0;
            return -1;
        }
        bundle_free(&b);
        j = romm_upload("POST", path, "saveFile", zip, f->upload_name, &st);
        unlink(zip);
    } else {
        j = romm_upload("POST", path, "saveFile", f->path, path_basename(f->path), &st);
    }
    if (status_out) *status_out = st;
    int ok = (st == 200 || st == 201) && j;
    if (ok) {
        record_save(f, (int)jget_num(j, "id", 0), jget_str(j, "content_hash", f->hash));
        LOGI("uploaded %s (rom %d, slot %s)", f->path, f->rom_id, f->slot);
    }
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

/* Where a downloaded save goes when the game has no local save yet. */
static void download_target(const local_rom *r, const char *server_name, const char *slot,
                            const scan_ctx *c, char *out, size_t n) {
    char ext[32];
    /* "autosave-rtc" holds a .rtc file */
    size_t sl = strlen(c->slot);
    if (!strncmp(slot, c->slot, sl) && slot[sl] == '-')
        snprintf(ext, sizeof ext, ".%s", slot + sl + 1);
    else {
        char first[32];
        str_copy(first, sizeof first, r->map->save_exts);
        char *comma = strchr(first, ',');
        if (comma) *comma = 0;
        str_copy(ext, sizeof ext, first[0] ? first : path_ext(server_name));
    }
    const char *vars[13];
    name_vars(r, ext, vars);
    char name[512], dir[PATH_MAX_LEN];
    str_template(name, sizeof name, r->map->save_name, vars);
    /* A name from a value this game lacks (no serial read) falls back to its own. */
    if (!name[0] || name[0] == '_' || name[0] == '.' || strchr(name, '{'))
        str_template(name, sizeof name, "{rom_stem}{ext}", vars);
    if (!expected_dir(r, 0, dir, sizeof dir)) str_copy(dir, sizeof dir, r->map->save_dir);
    path_join(out, n, dir, name);
}

static int do_download(const scan_ctx *c, int save_id, const char *dest, const local_file *f,
                       int rom_id, const char *slot, int session_id, const char *server_hash,
                       const char *server_updated_at) {
    char path[512], tmp[PATH_MAX_LEN];
    char *dev = url_q(c->device_id);
    int o = snprintf(path, sizeof path, "/api/saves/%d/content?device_id=%s&optimistic=false", save_id, dev);
    if (session_id) snprintf(path + o, sizeof path - (size_t)o, "&session_id=%d", session_id);
    free(dev);
    snprintf(tmp, sizeof tmp, "%s/tmp/save-%d.bin", plat_data_dir(), save_id);
    http_resp r;
    if (romm_download(path, tmp, NULL, NULL, &r) != 0) {
        LOGE("download of save %d failed: %s", save_id, r.error);
        unlink(tmp);
        return -1;
    }
    backup_file(c, dest, rom_id);
    if (move_file(tmp, dest) != 0) {
        LOGE("cannot write %s", dest);
        return -1;
    }
    time_t t = iso8601_parse(server_updated_at);
    if (t > 0) set_file_mtime(dest, t);

    local_file rec;
    memset(&rec, 0, sizeof rec);
    if (f) rec = *f;
    str_copy(rec.path, sizeof rec.path, dest);
    rec.rom_id = rom_id;
    str_copy(rec.slot, sizeof rec.slot, slot);
    char hash[33] = "";
    md5_file_hex(dest, hash);
    str_copy(rec.hash, sizeof rec.hash, hash);
    record_save(&rec, save_id, hash);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id", c->device_id);
    cJSON_AddStringToObject(body, "content_hash", server_hash && server_hash[0] ? server_hash : hash);
    snprintf(path, sizeof path, "/api/saves/%d/downloaded", save_id);
    long st = 0;
    cJSON_Delete(romm_call("POST", path, body, &st));
    if (st != 200) LOGW("RomM didn't take the confirmation for save %d (HTTP %ld); it may come down again", save_id, st);
    cJSON_Delete(body);
    LOGI("downloaded save %d -> %s", save_id, dest);
    return 0;
}

static const local_file *find_save(const scan_ctx *c, int rom_id, const char *slot) {
    for (int i = 0; i < c->nsaves; i++)
        if (c->saves[i].rom_id == rom_id && !strcmp(c->saves[i].slot, slot ? slot : "")) return &c->saves[i];
    return NULL;
}

static const local_rom *find_rom(const scan_ctx *c, int rom_id, const char *emulator) {
    const local_rom *any = NULL;
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (r->rom_id != rom_id || !r->rules.sync_saves) continue;
        if (emulator && str_ieq(r->map->emulator, emulator)) return r;
        if (!any) any = r;
    }
    return any;
}

static void record_conflict(const local_file *f, const cJSON *op) {
    state_lock();
    int is_new = state_entry("conflicts", f->path) == NULL;
    cJSON *e = state_entry_ensure("conflicts", f->path);
    jset_num(e, "rom_id", f->rom_id);
    jset_num(e, "save_id", jget_num(op, "save_id", 0));
    jset_str(e, "slot", f->slot);
    jset_str(e, "reason", jget_str(op, "reason", ""));
    jset_str(e, "server_updated_at", jget_str(op, "server_updated_at", ""));
    jset_str(e, "server_hash", jget_str(op, "server_content_hash", ""));
    jset_num(e, "local_mtime", (double)f->mtime);
    jset_num(e, "detected_at", (double)time(NULL));
    state_unlock();
    if (is_new) LOGW("conflict for %s: %s", f->path, jget_str(op, "reason", ""));
}


static int sync_states_up(scan_ctx *c) {
    int uploaded = 0;
    for (int i = 0; i < c->nstates; i++) {
        local_file *f = &c->states[i];
        if (file_mtime_ns(f->path) != f->mtime_ns) { /* replaced by the download pass */
            f->mtime = file_mtime(f->path);
            f->mtime_ns = file_mtime_ns(f->path);
            f->size = file_size(f->path);
            md5_file_hex(f->path, f->hash);
        }
        state_lock();
        cJSON *e = state_entry("states", f->path);
        int same = e && !strcmp(jget_str(e, "hash", ""), f->hash);
        int state_id = e ? (int)jget_num(e, "state_id", 0) : 0;
        state_unlock();
        if (same) continue;

        char path[512];
        long st = 0;
        cJSON *j = NULL;
        if (state_id) {
            snprintf(path, sizeof path, "/api/states/%d", state_id);
            j = romm_upload("PUT", path, "stateFile", f->path, path_basename(f->path), &st);
        }
        if (!state_id || st == 404) {
            cJSON_Delete(j);
            char *emu = url_q(f->map->emulator);
            snprintf(path, sizeof path, "/api/states?rom_id=%d&emulator=%s", f->rom_id, emu);
            free(emu);
            j = romm_upload("POST", path, "stateFile", f->path, path_basename(f->path), &st);
        }
        if ((st == 200 || st == 201) && j) {
            state_lock();
            cJSON *ne = state_entry_ensure("states", f->path);
            jset_num(ne, "rom_id", f->rom_id);
            jset_str(ne, "hash", f->hash);
            jset_num(ne, "mtime", (double)f->mtime);
            char ns[24];
            snprintf(ns, sizeof ns, "%lld", (long long)file_mtime_ns(f->path));
            jset_str(ne, "mtime_ns", ns);
            jset_num(ne, "size", (double)f->size);
            jset_num(ne, "state_id", jget_num(j, "id", state_id));
            jset_str(ne, "server_updated_at", jget_str(j, "updated_at", ""));
            jset_num(ne, "synced_at", (double)time(NULL));
            state_unlock();
            uploaded++;
            LOGI("uploaded state %s", f->path);
        }
        cJSON_Delete(j);
    }
    return uploaded;
}


/* Local file name for a state from the server. States made elsewhere may use
 * another name, so anything that doesn't start with the game's name gets it. */
static void state_local_name(const local_rom *r, const char *server_name, char *out, size_t n) {
    for (int i = 0; i < r->nstems; i++) {
        size_t l = strlen(r->stems[i]);
        if (!strncmp(server_name, r->stems[i], l) && server_name[l] == '.') {
            str_copy(out, n, server_name);
            return;
        }
    }
    const char *vars[13];
    name_vars(r, path_ext(server_name), vars);
    str_template(out, n, r->map->state_name, vars);
}

/* Downloads a state into place (backing up what was there) and records it. */
static int fetch_state(const scan_ctx *c, const cJSON *s, const char *path, int rom_id) {
    char api[128], tmp[PATH_MAX_LEN];
    int id = (int)jget_num(s, "id", 0);
    snprintf(api, sizeof api, "/api/states/%d/content", id);
    snprintf(tmp, sizeof tmp, "%s/tmp/state-%d.bin", plat_data_dir(), id);
    http_resp r;
    if (romm_download(api, tmp, NULL, NULL, &r) != 0) {
        unlink(tmp);
        return -1;
    }
    backup_file(c, path, rom_id);
    if (move_file(tmp, path) != 0) return -1;
    time_t t = iso8601_parse(jget_str(s, "updated_at", ""));
    if (t > 0) set_file_mtime(path, t);
    char hash[33] = "";
    md5_file_hex(path, hash);
    state_lock();
    cJSON *e = state_entry_ensure("states", path);
    jset_num(e, "rom_id", rom_id);
    jset_str(e, "hash", hash);
    jset_num(e, "mtime", (double)file_mtime(path));
    char ns[24];
    snprintf(ns, sizeof ns, "%lld", (long long)file_mtime_ns(path));
    jset_str(e, "mtime_ns", ns);
    jset_num(e, "size", (double)file_size(path));
    jset_num(e, "state_id", id);
    jset_str(e, "server_updated_at", jget_str(s, "updated_at", ""));
    jset_num(e, "synced_at", (double)time(NULL));
    state_unlock();
    LOGI("downloaded state %s", path);
    return 0;
}

/* Pulls states that changed on the server since we last saw them. A state
 * that also changed here is kept; the upload pass sends it. */
static int sync_states_down(scan_ctx *c) {
    long st = 0;
    cJSON *list = romm_call("GET", "/api/states", NULL, &st);
    int n = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, list) {
        const local_rom *r = find_rom(c, (int)jget_num(s, "rom_id", 0), jget_str(s, "emulator", NULL));
        char dir[PATH_MAX_LEN], name[256], path[PATH_MAX_LEN];
        if (!r || !expected_dir(r, 1, dir, sizeof dir)) continue;
        const char *server_name = jget_str(s, "file_name", "");
        if (!ext_in_list(server_name, r->map->state_exts)) continue;
        state_local_name(r, server_name, name, sizeof name);
        path_join(path, sizeof path, dir, name);

        const char *server_up = jget_str(s, "updated_at", "");
        state_lock();
        cJSON *e = state_entry("states", path);
        int known = e && !strcmp(jget_str(e, "server_updated_at", ""), server_up);
        char rec_hash[33];
        str_copy(rec_hash, sizeof rec_hash, e ? jget_str(e, "hash", "") : "");
        state_unlock();
        if (known) continue;

        if (file_exists(path)) {
            char hash[33];
            md5_file_hex(path, hash);
            if (rec_hash[0] ? strcmp(hash, rec_hash) != 0 : file_mtime(path) >= iso8601_parse(server_up)) continue;
        }
        if (fetch_state(c, s, path, r->rom_id) == 0) n++;
    }
    cJSON_Delete(list);
    return n;
}

typedef struct {
    int uploaded, downloaded, conflicts, skipped, failed, states_up, states_down, noop, deferred, cards;
    char error[256];
} sync_result;

/* A game played by several emulators that keep its save in the same format
 * (save_format) has one save: the newest of their copies goes to the others,
 * the copies replaced backed up first. Runs before the saves are scanned (so
 * one save goes up) and after the downloads (so a new one reaches them all). */
static int share_saves(scan_ctx *c) {
    int copied = 0;
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *a = &c->roms[i];
        if (!a->map->save_format[0] || a->map->own_slot || !a->rules.sync_saves) continue;
        /* The first member of each group does the work. */
        int first = 1;
        for (int k = 0; k < i && first; k++)
            if (c->roms[k].rom_id == a->rom_id && !strcmp(c->roms[k].map->save_format, a->map->save_format) &&
                !c->roms[k].map->own_slot && c->roms[k].rules.sync_saves)
                first = 0;
        if (!first) continue;
        char paths[8][PATH_MAX_LEN];
        int n = 0, newest = -1;
        time_t newest_t = 0;
        for (int k = i; k < c->nroms && n < 8; k++) {
            const local_rom *r = &c->roms[k];
            if (r->rom_id != a->rom_id || strcmp(r->map->save_format, a->map->save_format) || r->map->own_slot ||
                !r->rules.sync_saves)
                continue;
            download_target(r, "", c->slot, c, paths[n], sizeof paths[n]);
            time_t t = file_mtime(paths[n]);
            if (t > newest_t) newest_t = t, newest = n;
            n++;
        }
        if (n < 2 || newest < 0) continue;
        char want[33];
        if (md5_file_hex(paths[newest], want) != 0) continue;
        for (int k = 0; k < n; k++) {
            char have[33] = "";
            if (k == newest || (md5_file_hex(paths[k], have) == 0 && !strcmp(have, want))) continue;
            if (file_exists(paths[k])) backup_file(c, paths[k], a->rom_id);
            mkdir_parent(paths[k]);
            if (copy_file(paths[newest], paths[k]) == 0) {
                set_file_mtime(paths[k], newest_t);
                LOGI("shared save: %s -> %s", paths[newest], paths[k]);
                copied++;
            }
        }
    }
    return copied;
}

/* Game ids, save ids and what was synced belong to one RomM server (and one
 * device on it). Paired with another, they mean nothing there: they're
 * forgotten, and the games are matched again by name. */
static void server_id(char *out, size_t n) {
    config_lock();
    snprintf(out, n, "%s|%s", g_cfg.server_url, g_cfg.device_id);
    config_unlock();
}

static void forget_server_records(void) {
    static const char *const sections[] = {"roms", "saves", "states", "conflicts", "memcards"};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        cJSON *sec = state_section(sections[i]);
        while (sec->child) cJSON_Delete(cJSON_DetachItemViaPointer(sec, sec->child));
    }
}

void sync_new_pairing(void) {
    char id[700];
    server_id(id, sizeof id);
    state_lock();
    forget_server_records();
    jset_str(state_section("server"), "id", id);
    state_save();
    state_unlock();
    LOGI("new pairing: the games are matched again");
}

/* The same check at every sync, for a config changed some other way. A state
 * from before servers were recorded belongs to the server paired now. */
static void forget_other_server(void) {
    char id[700];
    server_id(id, sizeof id);
    state_lock();
    cJSON *meta = state_section("server");
    const char *was = jget_str(meta, "id", NULL);
    if (was && strcmp(was, id) != 0) {
        forget_server_records();
        LOGI("paired with another RomM server: matching the games again");
    }
    jset_str(meta, "id", id);
    state_save();
    state_unlock();
}

static void run_sync(const char *reason, sync_result *res) {
    memset(res, 0, sizeof *res);
    forget_other_server();
    scan_ctx c;
    set_phase("scanning local files");
    if (scan_prepare(&c) != 0) {
        snprintf(res->error, sizeof res->error, "cannot reach RomM server");
        scan_free(&c);
        return;
    }
    share_saves(&c);
    scan_saves(&c);
    LOGI("sync (%s): %d local roms, %d saves, %d states", reason, c.nroms, c.nsaves, c.nstates);

    set_phase("negotiating with server");
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id", c.device_id);
    cJSON *arr = cJSON_AddArrayToObject(body, "saves");
    for (int i = 0; i < c.nsaves; i++) {
        local_file *f = &c.saves[i];
        char ts[40];
        iso8601_utc(f->mtime, ts, sizeof ts);
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "rom_id", f->rom_id);
        cJSON_AddStringToObject(s, "file_name", f->is_bundle ? f->upload_name : path_basename(f->path));
        cJSON_AddStringToObject(s, "slot", f->slot);
        cJSON_AddStringToObject(s, "emulator", f->map->emulator);
        cJSON_AddStringToObject(s, "content_hash", f->hash);
        cJSON_AddStringToObject(s, "updated_at", ts);
        cJSON_AddNumberToObject(s, "file_size_bytes", (double)f->size);
        cJSON_AddItemToArray(arr, s);
    }
    long st = 0;
    cJSON *neg = romm_call("POST", "/api/sync/negotiate", body, &st);
    cJSON_Delete(body);
    if (st != 200 || !neg) {
        char d[200];
        romm_error_detail(neg, d, sizeof d);
        snprintf(res->error, sizeof res->error, "negotiate failed (HTTP %ld) %s", st, d);
        cJSON_Delete(neg);
        scan_free(&c);
        return;
    }
    int session_id = (int)jget_num(neg, "session_id", 0);
    /* A running game would overwrite a downloaded save, so downloads wait until
     * it closes. The sync right after launch still downloads. */
    int upload_only = g_game_running && strcmp(reason, "game started") != 0;

    const cJSON *op;
    int nops = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(neg, "operations")), k = 0;
    int last_rom = 0, roms_touched = 0;
    cJSON_ArrayForEach(op, cJSON_GetObjectItemCaseSensitive(neg, "operations")) {
        char ph[64];
        snprintf(ph, sizeof ph, "syncing saves (%d/%d)", ++k, nops);
        set_phase(ph);
        const char *action = jget_str(op, "action", "");
        int rom_id = (int)jget_num(op, "rom_id", 0);
        int save_id = (int)jget_num(op, "save_id", 0);
        const char *slot = jget_str(op, "slot", "");
        const local_file *f = find_save(&c, rom_id, slot);

        const char *server_hash = jget_str(op, "server_content_hash", "");
        if (!strcmp(action, "no_op") && f && server_hash[0] && strcmp(server_hash, f->hash) != 0) {
            /* RomM thinks in whole seconds, so a change right after a sync can
             * come back as "nothing to do" while the hashes disagree. Trust our
             * own record of which side moved instead. */
            state_lock();
            cJSON *e = state_entry("saves", f->path);
            int local_changed = !e || strcmp(jget_str(e, "hash", ""), f->hash) != 0;
            state_unlock();
            action = local_changed ? "upload" : "download";
        }
        if (!strcmp(action, "no_op")) {
            if (f) {
                state_lock();
                cJSON *e = state_entry("saves", f->path);
                int changed = !e || strcmp(jget_str(e, "hash", ""), f->hash);
                state_unlock();
                if (changed) record_save(f, save_id, NULL);
                else {
                    state_lock();
                    state_entry_remove("conflicts", f->path);
                    state_unlock();
                }
            }
            res->noop++;
            continue;
        }

        int want_up = !strcmp(action, "upload"), want_down = !strcmp(action, "download");
        int overwrite = 0, is_conflict = !strcmp(action, "conflict");
        if (want_down && f) {
            /* With no sync history RomM just goes by timestamps, which is how you
             * lose a save. Unsynced local changes are never overwritten; call it
             * a conflict and let a human decide. */
            state_lock();
            cJSON *e = state_entry("saves", f->path);
            int local_dirty = !e || strcmp(jget_str(e, "hash", ""), f->hash) != 0;
            state_unlock();
            if (local_dirty) {
                is_conflict = 1;
                want_down = 0;
            }
        }
        if (is_conflict && f) {
            conflict_policy p = c.policy;
            if (p == CONFLICT_NEWEST) {
                time_t srv = iso8601_parse(jget_str(op, "server_updated_at", ""));
                p = f->mtime >= srv ? CONFLICT_LOCAL : CONFLICT_SERVER;
            }
            if (p == CONFLICT_LOCAL) want_up = overwrite = 1;
            else if (p == CONFLICT_SERVER && upload_only) {
                res->deferred++;
                continue;
            } else if (p == CONFLICT_SERVER) want_down = 1;
            else {
                record_conflict(f, op);
                res->conflicts++;
                continue;
            }
        }

        if (want_up && f) {
            long ust = 0;
            if (do_upload(&c, f, session_id, overwrite, &ust) == 0) {
                res->uploaded++;
                if (last_rom != rom_id) roms_touched++;
                last_rom = rom_id;
            } else if (ust == 409) {
                /* The server copy changed since negotiate. */
                record_conflict(f, op);
                res->conflicts++;
            } else {
                res->failed++;
            }
        } else if (want_down && upload_only) {
            res->deferred++;
        } else if (want_down) {
            const local_rom *r = f ? f->rom : find_rom(&c, rom_id, jget_str(op, "emulator", NULL));
            if (!r) {
                res->skipped++; /* game isn't on this console */
                continue;
            }
            char dest[PATH_MAX_LEN];
            int ok;
            if (r->map->save_layout[0]) {
                ok = do_download_bundle(&c, save_id, r, slot, session_id, jget_str(op, "server_content_hash", ""), 1) == 0;
            } else {
                if (f)
                    str_copy(dest, sizeof dest, f->path);
                else
                    download_target(r, jget_str(op, "file_name", ""), slot, &c, dest, sizeof dest);
                ok = do_download(&c, save_id, dest, f, rom_id, slot, session_id, jget_str(op, "server_content_hash", ""),
                                 jget_str(op, "server_updated_at", "")) == 0;
            }
            if (ok) {
                res->downloaded++;
            } else {
                res->failed++;
            }
        } else {
            res->skipped++;
        }
    }
    cJSON_Delete(neg);

    set_phase("finishing");
    cJSON *done = cJSON_CreateObject();
    cJSON_AddNumberToObject(done, "operations_completed", res->uploaded + res->downloaded + res->noop);
    cJSON_AddNumberToObject(done, "operations_failed", res->failed);
    pthread_mutex_lock(&g_req_lock);
    cJSON *ps = g_play_sessions;
    g_play_sessions = NULL;
    pthread_mutex_unlock(&g_req_lock);
    if (ps) {
        /* If one game's save changed, that's the game that was played. */
        cJSON *e;
        cJSON_ArrayForEach(e, ps) {
            cJSON_DeleteItemFromObjectCaseSensitive(e, "title_id");
            if (roms_touched == 1) cJSON_AddNumberToObject(e, "rom_id", last_rom);
            cJSON_AddStringToObject(e, "save_slot", c.slot);
        }
        cJSON_AddItemToObject(done, "play_sessions", ps);
    }
    char p[128];
    snprintf(p, sizeof p, "/api/sync/sessions/%d/complete", session_id);
    long cst = 0;
    cJSON_Delete(romm_call("POST", p, done, &cst));
    cJSON_Delete(done);
    if (cst != 200) LOGW("session complete returned HTTP %ld", cst);

    int do_states;
    config_lock();
    do_states = g_cfg.sync_states;
    config_unlock();
    if (do_states) {
        set_phase("syncing save states");
        if (do_states == 2 && !upload_only) res->states_down = sync_states_down(&c);
        res->states_up = sync_states_up(&c);
    }

    res->cards = memcard_backup();
    /* A save that just came down reaches the other emulators that share it. */
    share_saves(&c);

    state_lock();
    state_save();
    state_unlock();
    scan_free(&c);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        char reason[64];
        pthread_mutex_lock(&g_req_lock);
        while (!g_req_reason[0]) pthread_cond_wait(&g_req_cond, &g_req_lock);
        str_copy(reason, sizeof reason, g_req_reason);
        g_req_reason[0] = 0;
        pthread_mutex_unlock(&g_req_lock);

        if (!config_is_paired()) continue;
        /* Only manual syncs run until setup is finished. */
        int ready;
        config_lock();
        ready = g_cfg.setup_complete;
        config_unlock();
        if (!ready && strcmp(reason, "manual") != 0 && strcmp(reason, "first sync") != 0) {
            LOGI("sync (%s) skipped: setup not finished", reason);
            continue;
        }
        pthread_mutex_lock(&g_run_lock);
        g_running = 1;
        sync_result r;
        run_sync(reason, &r);
        g_running = 0;
        g_last_run = time(NULL);
        set_phase("idle");
        pthread_mutex_unlock(&g_run_lock);

        cJSON *h = cJSON_CreateObject();
        g_sync_count++;
        cJSON_AddNumberToObject(h, "time", (double)g_last_run);
        cJSON_AddStringToObject(h, "reason", reason);
        cJSON_AddNumberToObject(h, "uploaded", r.uploaded);
        cJSON_AddNumberToObject(h, "downloaded", r.downloaded);
        cJSON_AddNumberToObject(h, "conflicts", r.conflicts);
        cJSON_AddNumberToObject(h, "skipped", r.skipped);
        cJSON_AddNumberToObject(h, "failed", r.failed);
        cJSON_AddNumberToObject(h, "states_uploaded", r.states_up);
        cJSON_AddNumberToObject(h, "states_downloaded", r.states_down);
        cJSON_AddNumberToObject(h, "deferred", r.deferred);
        cJSON_AddNumberToObject(h, "cards_uploaded", r.cards);
        if (r.error[0]) cJSON_AddStringToObject(h, "error", r.error);
        state_lock();
        state_history_add(h);
        state_save();
        state_unlock();

        int notify;
        config_lock();
        notify = g_cfg.notify;
        config_unlock();
        if (r.error[0]) {
            LOGE("sync failed: %s", r.error);
            if (notify) plat_notify("RomM Sync: sync failed: %s", r.error);
        } else {
            LOGI("sync done: %d up, %d down, %d conflicts, %d failed, %d states", r.uploaded,
                 r.downloaded, r.conflicts, r.failed, r.states_up);
            if (notify && (r.uploaded || r.downloaded || r.conflicts || r.failed))
                plat_notify("RomM Sync: %d uploaded, %d downloaded%s", r.uploaded, r.downloaded,
                            r.conflicts ? ", conflicts to resolve" : "");
        }
    }
    return NULL;
}

void sync_init(void) {
    thread_start(worker, NULL);
}

void sync_request(const char *reason) {
    pthread_mutex_lock(&g_req_lock);
    if (!g_req_reason[0]) str_copy(g_req_reason, sizeof g_req_reason, reason);
    pthread_cond_signal(&g_req_cond);
    pthread_mutex_unlock(&g_req_lock);
}

int sync_is_running(void) { return g_running; }
time_t sync_last_run(void) { return g_last_run; }

void sync_add_play_session(time_t start, time_t end, const char *title_id) {
    if (end - start < 60) return;
    char a[40], b[40];
    iso8601_utc(start, a, sizeof a);
    iso8601_utc(end, b, sizeof b);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "start_time", a);
    cJSON_AddStringToObject(e, "end_time", b);
    cJSON_AddNumberToObject(e, "duration_ms", (double)(end - start) * 1000.0);
    cJSON_AddStringToObject(e, "title_id", title_id ? title_id : "");
    pthread_mutex_lock(&g_req_lock);
    if (!g_play_sessions) g_play_sessions = cJSON_CreateArray();
    cJSON_AddItemToArray(g_play_sessions, e);
    pthread_mutex_unlock(&g_req_lock);
}

cJSON *sync_status_json(void) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "running", g_running);
    cJSON_AddStringToObject(j, "phase", g_phase);
    cJSON_AddNumberToObject(j, "last_run", (double)g_last_run);
    cJSON_AddNumberToObject(j, "sync_count", g_sync_count);
    state_lock();
    cJSON_AddItemToObject(j, "history", cJSON_Duplicate(state_section("history"), 1));
    cJSON *conf = cJSON_CreateArray();
    cJSON *e;
    cJSON_ArrayForEach(e, state_section("conflicts")) {
        cJSON *c = cJSON_Duplicate(e, 1);
        cJSON_AddStringToObject(c, "path", e->string);
        cJSON *rom = NULL, *r;
        cJSON_ArrayForEach(r, state_section("roms"))
            if ((int)jget_num(r, "rom_id", -1) == (int)jget_num(e, "rom_id", 0)) rom = r;
        cJSON_AddStringToObject(c, "rom_name", rom ? jget_str(rom, "name", "") : "");
        cJSON_AddItemToArray(conf, c);
    }
    cJSON_AddItemToObject(j, "conflicts", conf);
    cJSON_AddItemToObject(j, "unmatched", g_unmatched ? cJSON_Duplicate(g_unmatched, 1) : cJSON_CreateArray());
    cJSON_AddItemToObject(j, "strays", g_strays ? cJSON_Duplicate(g_strays, 1) : cJSON_CreateArray());
    int tracked = cJSON_GetArraySize(state_section("saves"));
    state_unlock();
    cJSON_AddNumberToObject(j, "tracked_saves", tracked);
    return j;
}

int sync_resolve_conflict(const char *path, const char *keep, char *err, int en) {
    int rc = -1;
    int local = !strcmp(keep, "local"), server = !strcmp(keep, "server");
    if (!local && !server) {
        snprintf(err, (size_t)en, "keep must be local or server");
        return -1;
    }
    pthread_mutex_lock(&g_run_lock);
    state_lock();
    cJSON *e = cJSON_Duplicate(state_entry("conflicts", path), 1);
    state_unlock();
    scan_ctx c;
    memset(&c, 0, sizeof c);
    if (!e) {
        snprintf(err, (size_t)en, "no such conflict");
        goto done;
    }
    if (scan_prepare(&c) != 0) {
        snprintf(err, (size_t)en, "cannot reach RomM");
        goto done;
    }
    scan_saves(&c);
    const local_file *f = NULL;
    for (int i = 0; i < c.nsaves && !f; i++)
        if (!strcmp(c.saves[i].path, path)) f = &c.saves[i];
    if (!f) {
        snprintf(err, (size_t)en, "the local save is gone");
        goto done;
    }
    if (local) {
        long st = 0;
        rc = do_upload(&c, f, 0, 1, &st);
        if (rc) snprintf(err, (size_t)en, "upload failed (HTTP %ld)", st);
    } else if (f->is_bundle) {
        rc = do_download_bundle(&c, (int)jget_num(e, "save_id", 0), f->rom, f->slot, 0, jget_str(e, "server_hash", ""), 1);
        if (rc) snprintf(err, (size_t)en, "download failed");
    } else {
        rc = do_download(&c, (int)jget_num(e, "save_id", 0), path, f, f->rom_id, f->slot, 0, jget_str(e, "server_hash", ""),
                         jget_str(e, "server_updated_at", ""));
        if (rc) snprintf(err, (size_t)en, "download failed");
    }
    if (rc == 0) {
        state_lock();
        state_entry_remove("conflicts", path);
        state_save();
        state_unlock();
    }
done:
    cJSON_Delete(e);
    scan_free(&c);
    pthread_mutex_unlock(&g_run_lock);
    return rc;
}

int sync_move_stray(const char *path, char *err, int en) {
    int rc = -1;
    pthread_mutex_lock(&g_run_lock);
    char dest[PATH_MAX_LEN] = "";
    int rom_id = 0;
    state_lock();
    const cJSON *e;
    cJSON_ArrayForEach(e, g_strays) {
        if (!strcmp(jget_str(e, "path", ""), path)) {
            str_copy(dest, sizeof dest, jget_str(e, "expected_path", ""));
            rom_id = (int)jget_num(e, "rom_id", 0);
        }
    }
    state_unlock();
    int keep;
    config_lock();
    keep = g_cfg.keep_backups;
    config_unlock();
    if (!dest[0]) {
        snprintf(err, (size_t)en, "not a stray save (run a sync first)");
    } else if (file_exists(dest)) {
        snprintf(err, (size_t)en, "the emulator already has a save at %s; not overwriting it", dest);
    } else if (!file_exists(path)) {
        snprintf(err, (size_t)en, "file no longer exists");
    } else {
        scan_ctx c;
        memset(&c, 0, sizeof c);
        c.keep_backups = keep > 0 ? keep : 1;
        backup_file(&c, path, rom_id);
        if (move_file(path, dest) == 0) {
            LOGI("moved stray save %s -> %s", path, dest);
            rc = 0;
        } else {
            snprintf(err, (size_t)en, "could not move to %s", dest);
        }
    }
    pthread_mutex_unlock(&g_run_lock);
    if (rc == 0) sync_request("stray moved");
    return rc;
}

void sync_set_game_running(int running) { g_game_running = running; }
int sync_game_running(void) { return g_game_running; }


static const char *rom_display_name(const local_rom *r, char *buf, size_t n) {
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    str_copy(buf, n, e ? jget_str(e, "name", "") : "");
    state_unlock();
    if (!buf[0]) path_stem(buf, n, r->path);
    return buf;
}

static void preview_item(cJSON *arr, const char *game, const char *path, const char *note) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "game", game);
    cJSON_AddStringToObject(o, "path", path);
    if (note) cJSON_AddStringToObject(o, "note", note);
    cJSON_AddItemToArray(arr, o);
}

cJSON *sync_preview(char *err, int en) {
    pthread_mutex_lock(&g_run_lock);
    scan_ctx c;
    cJSON *out = NULL;
    if (scan_prepare(&c) != 0) {
        snprintf(err, (size_t)en, "cannot reach the RomM server");
        scan_free(&c);
        pthread_mutex_unlock(&g_run_lock);
        return NULL;
    }
    scan_saves(&c);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id", c.device_id);
    cJSON *arr = cJSON_AddArrayToObject(body, "saves");
    for (int i = 0; i < c.nsaves; i++) {
        local_file *f = &c.saves[i];
        char ts[40];
        iso8601_utc(f->mtime, ts, sizeof ts);
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "rom_id", f->rom_id);
        cJSON_AddStringToObject(s, "file_name", f->is_bundle ? f->upload_name : path_basename(f->path));
        cJSON_AddStringToObject(s, "slot", f->slot);
        cJSON_AddStringToObject(s, "emulator", f->map->emulator);
        cJSON_AddStringToObject(s, "content_hash", f->hash);
        cJSON_AddStringToObject(s, "updated_at", ts);
        cJSON_AddNumberToObject(s, "file_size_bytes", (double)f->size);
        cJSON_AddItemToArray(arr, s);
    }
    long st = 0;
    cJSON *neg = romm_call("POST", "/api/sync/negotiate", body, &st);
    cJSON_Delete(body);
    if (st != 200 || !neg) {
        char d[200];
        romm_error_detail(neg, d, sizeof d);
        snprintf(err, (size_t)en, "negotiate failed (HTTP %ld) %s", st, d);
        cJSON_Delete(neg);
        scan_free(&c);
        pthread_mutex_unlock(&g_run_lock);
        return NULL;
    }

    out = cJSON_CreateObject();
    cJSON *up = cJSON_AddArrayToObject(out, "uploads");
    cJSON *down = cJSON_AddArrayToObject(out, "downloads");
    cJSON *conf = cJSON_AddArrayToObject(out, "conflicts");
    int same = 0, elsewhere = 0;
    const cJSON *op;
    cJSON_ArrayForEach(op, cJSON_GetObjectItemCaseSensitive(neg, "operations")) {
        const char *action = jget_str(op, "action", "");
        int rom_id = (int)jget_num(op, "rom_id", 0);
        const char *slot = jget_str(op, "slot", "");
        const local_file *f = find_save(&c, rom_id, slot);
        char game[256] = "";
        if (f) rom_display_name(f->rom, game, sizeof game);
        if (!strcmp(action, "no_op")) {
            same++;
        } else if (!strcmp(action, "upload") && f) {
            preview_item(up, game, f->path, NULL);
        } else if (!strcmp(action, "conflict") && f) {
            preview_item(conf, game, f->path, jget_str(op, "reason", ""));
        } else if (!strcmp(action, "download")) {
            if (f) {
                state_lock();
                cJSON *e = state_entry("saves", f->path);
                int dirty = !e || strcmp(jget_str(e, "hash", ""), f->hash) != 0;
                state_unlock();
                if (dirty)
                    preview_item(conf, game, f->path, "Different save on the server and on this console");
                else
                    preview_item(down, game, f->path, "replaces the local save (a backup is kept)");
                continue;
            }
            const local_rom *r = find_rom(&c, rom_id, jget_str(op, "emulator", NULL));
            if (!r) {
                elsewhere++;
                continue;
            }
            char dest[PATH_MAX_LEN];
            if (r->map->save_layout[0]) {
                char bases[6][PATH_MAX_LEN];
                int nb = bundle_bases(r, bases, 6);
                str_copy(dest, sizeof dest, nb ? bases[0] : r->rules.save_dir);
            } else {
                download_target(r, jget_str(op, "file_name", ""), slot, &c, dest, sizeof dest);
            }
            rom_display_name(r, game, sizeof game);
            preview_item(down, game, dest, NULL);
        }
    }
    cJSON_AddNumberToObject(out, "in_sync", same);
    cJSON_AddNumberToObject(out, "not_on_console", elsewhere);
    cJSON_AddNumberToObject(out, "local_games", c.nroms);
    cJSON_AddNumberToObject(out, "local_saves", c.nsaves);
    state_lock();
    cJSON_AddItemToObject(out, "strays", cJSON_Duplicate(g_strays, 1));
    cJSON_AddItemToObject(out, "unmatched", cJSON_Duplicate(g_unmatched, 1));
    state_save();
    state_unlock();

    /* Close the session without doing anything. */
    char p[128];
    snprintf(p, sizeof p, "/api/sync/sessions/%d/complete", (int)jget_num(neg, "session_id", 0));
    cJSON *done = cJSON_CreateObject();
    cJSON_AddNumberToObject(done, "operations_completed", 0);
    cJSON_AddNumberToObject(done, "operations_failed", 0);
    cJSON_Delete(romm_call("POST", p, done, NULL));
    cJSON_Delete(done);
    cJSON_Delete(neg);
    scan_free(&c);
    pthread_mutex_unlock(&g_run_lock);
    return out;
}

int sync_scanned_dirs(char dirs[][1024], int *any_file, int max) {
    int n = 0;
    state_lock();
    const cJSON *lists[2] = {g_roots, g_bundle_roots};
    for (int k = 0; k < 2; k++) {
        const cJSON *d;
        cJSON_ArrayForEach(d, lists[k]) {
            if (n >= max) break;
            if (!cJSON_IsString(d)) continue;
            any_file[n] = k == 1;
            str_copy(dirs[n++], 1024, d->valuestring);
        }
    }
    state_unlock();
    return n;
}

cJSON *sync_history(int rom_id, char *err, int en) {
    char path[128];
    long st1 = 0, st2 = 0;
    snprintf(path, sizeof path, "/api/saves?rom_id=%d", rom_id);
    cJSON *saves = romm_call("GET", path, NULL, &st1);
    snprintf(path, sizeof path, "/api/states?rom_id=%d", rom_id);
    cJSON *states = romm_call("GET", path, NULL, &st2);
    if (st1 != 200 || st2 != 200) {
        snprintf(err, (size_t)en, "could not load history from RomM");
        cJSON_Delete(saves);
        cJSON_Delete(states);
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    const char *kinds[2] = {"saves", "states"};
    cJSON *lists[2] = {saves, states};
    for (int k = 0; k < 2; k++) {
        cJSON *arr = cJSON_AddArrayToObject(out, kinds[k]);
        const cJSON *s;
        cJSON_ArrayForEach(s, lists[k]) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", jget_num(s, "id", 0));
            cJSON_AddStringToObject(o, "file_name", jget_str(s, "file_name", ""));
            cJSON_AddStringToObject(o, "slot", jget_str(s, "slot", ""));
            cJSON_AddStringToObject(o, "emulator", jget_str(s, "emulator", ""));
            cJSON_AddStringToObject(o, "updated_at", jget_str(s, "updated_at", ""));
            cJSON_AddNumberToObject(o, "size", jget_num(s, "file_size_bytes", 0));
            const char *device = "";
            const cJSON *d;
            cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(s, "device_syncs"))
                if (!strcmp(jget_str(d, "device_id", ""), jget_str(s, "origin_device_id", "-"))) device = jget_str(d, "device_name", "");
            cJSON_AddStringToObject(o, "device", device);
            cJSON_AddItemToArray(arr, o);
        }
    }
    cJSON_Delete(saves);
    cJSON_Delete(states);
    return out;
}

int sync_restore(const char *kind, int id, char *err, int en) {
    int is_state = !strcmp(kind, "state");
    if (!is_state && strcmp(kind, "save") != 0) {
        snprintf(err, (size_t)en, "kind must be save or state");
        return -1;
    }
    if (!is_state && sync_game_running()) {
        snprintf(err, (size_t)en, "close the game before restoring a save");
        return -1;
    }
    char api[128];
    snprintf(api, sizeof api, is_state ? "/api/states/%d" : "/api/saves/%d", id);
    long st = 0;
    cJSON *meta = romm_call("GET", api, NULL, &st);
    if (st != 200 || !meta) {
        snprintf(err, (size_t)en, "not found on RomM");
        cJSON_Delete(meta);
        return -1;
    }

    pthread_mutex_lock(&g_run_lock);
    scan_ctx c;
    int rc = -1;
    if (scan_prepare(&c) != 0) {
        snprintf(err, (size_t)en, "cannot reach RomM");
        goto done;
    }
    scan_saves(&c);
    int rom_id = (int)jget_num(meta, "rom_id", 0);
    const local_rom *r = find_rom(&c, rom_id, jget_str(meta, "emulator", NULL));
    if (!r) {
        snprintf(err, (size_t)en, "the game isn't on this console");
        goto done;
    }
    char dest[PATH_MAX_LEN], dir[PATH_MAX_LEN], name[256];
    if (is_state) {
        if (!expected_dir(r, 1, dir, sizeof dir)) {
            snprintf(err, (size_t)en, "unknown state folder");
            goto done;
        }
        state_local_name(r, jget_str(meta, "file_name", ""), name, sizeof name);
        path_join(dest, sizeof dest, dir, name);
        rc = fetch_state(&c, meta, dest, rom_id);
        if (rc) snprintf(err, (size_t)en, "download failed");
    } else if (r->map->save_layout[0]) {
        const char *slot = jget_str(meta, "slot", c.slot);
        rc = do_download_bundle(&c, id, r, slot && slot[0] ? slot : c.slot, 0, "", 0);
        if (rc) snprintf(err, (size_t)en, "download failed");
        str_copy(dest, sizeof dest, r->path);
    } else {
        const char *slot = jget_str(meta, "slot", c.slot);
        const local_file *f = find_save(&c, rom_id, slot && slot[0] ? slot : c.slot);
        if (f)
            str_copy(dest, sizeof dest, f->path);
        else
            download_target(r, jget_str(meta, "file_name", ""), slot ? slot : c.slot, &c, dest, sizeof dest);
        char tmp[PATH_MAX_LEN];
        snprintf(api, sizeof api, "/api/saves/%d/content", id);
        snprintf(tmp, sizeof tmp, "%s/tmp/restore-save-%d.bin", plat_data_dir(), id);
        http_resp resp;
        if (romm_download(api, tmp, NULL, NULL, &resp) != 0) {
            snprintf(err, (size_t)en, "download failed");
            unlink(tmp);
            goto done;
        }
        backup_file(&c, dest, rom_id);
        rc = move_file(tmp, dest);
        if (rc) snprintf(err, (size_t)en, "could not write %s", dest);
        /* Not recorded as synced: the next sync uploads it as the newest version. */
    }
    if (rc == 0) LOGI("restored %s %d to %s", kind, id, dest);
done:
    scan_free(&c);
    pthread_mutex_unlock(&g_run_lock);
    cJSON_Delete(meta);
    if (rc == 0) sync_request("restore");
    return rc;
}
