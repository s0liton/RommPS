#include "sync.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "bundle.h"
#include "gcmem.h"
#include "gameid.h"
#include "http.h"
#include "memcard.h"
#include "n64.h"
#include "platform.h"
#include "profiles.h"
#include "ps2card.h"
#include "romm.h"
#include "events.h"
#include "firmware.h"
#include "state.h"
#include "statefmt.h"
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
    char n64_md5[33];     /* an N64 ROM's MD5 as mupen64plus computes it */
    char crc[9];          /* a PS2 disc's, as PCSX2 names its states: "248E6126" */
    char safe_stem[256];  /* stems[0], non-alphanumerics as "_" */
    char game_id[24];     /* a disc's, "GAFE01" (DolphinPS4 names states by it), or a title's */
    rom_rules rules;
} local_rom;

static int rom_game_id(const local_rom *r, char *out, size_t n);
static const char *emu_name(const profile_map *m);

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
    char game_id[24];
    char upload_name[256];
    /* GameCube saves in a GCI folder keep the form RomM has them in (gc_form):
     * "zip" (the folder's files for the game), "gci" (one of them, gc_file)
     * or "card" (a whole memory card with them on it). */
    char gc_form[8];
    char gc_file[PATH_MAX_LEN];
    char ps2_identity[33]; /* ps2: the saves' identity (ps2card_collect) */
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
    long server_status; /* of the platform list; 0 when the server never answered */
    char slot[32], device_id[64];
    int keep_backups;
    int server_versions;
    conflict_policy policy;
    cJSON *romm_saves; /* RomM's saves of each game asked about, this sync: {"<rom id>": [...]} */
    cJSON *romm_has;   /* the games RomM has any save of, from negotiate: {"<rom id>": true}; NULL: unknown */
} scan_ctx;

static pthread_mutex_t g_run_lock = PTHREAD_MUTEX_INITIALIZER; /* one sync at a time */

/* Saves this pass put in place from RomM, or kept in a conflict: the ones
 * sharing gives the others, whatever the dates say. A game that writes its
 * save as it starts leaves an older copy with a newer date (PS5N64's, beside
 * the RomM save just chosen for RetroArch). Under g_run_lock. */
static char g_fresh[16][PATH_MAX_LEN];
static int g_nfresh;

static void mark_fresh(const char *path) {
    for (int i = 0; i < g_nfresh; i++)
        if (!strcmp(g_fresh[i], path)) return;
    if (g_nfresh < (int)(sizeof g_fresh / sizeof *g_fresh)) str_copy(g_fresh[g_nfresh++], sizeof g_fresh[0], path);
}

static int is_fresh(const char *path) {
    for (int i = 0; i < g_nfresh; i++)
        if (!strcmp(g_fresh[i], path)) return 1;
    return 0;
}
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
/* The lists the sync running now fills, swapped in when its scan is done:
 * the watcher never sees one half made (its folders gone for a moment, their
 * saves back as if new, and a sync for nothing). */
static cJSON *g_roots_next, *g_bundle_roots_next;
static cJSON *g_bundle_roots;  /* bundle folders, watched for any file, guarded by state_lock */
static volatile int g_game_running;
static volatile int g_sync_count;

static void set_phase(const char *p) { str_copy(g_phase, sizeof g_phase, p); }


static char *url_q(const char *s) { return http_escape(s ? s : ""); }

/* This console's slots: its own ("autosave"), and the ones derived from it
 * ("autosave-rtc", "autosave-<emulator>"). Other slots are other devices'. */
static int our_slot(const scan_ctx *c, const char *slot) {
    size_t n = strlen(c->slot);
    return !strncmp(slot, c->slot, n) && (slot[n] == 0 || slot[n] == '-');
}

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
    if (!first[0] || str_ieq(ext, first) || str_ieq(path_basename(file), first)) {
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
           strstr(m->save_names_by_core, var) ||
           strstr(m->state_name, var) || strstr(m->resume_states, var);
}

/* The template variables a game's file names can use. */
static void name_vars(const local_rom *r, const char *ext, const char *vars[17]) {
    /* An unknown CRC stays a placeholder, so a name made with it names nothing. */
    const char *v[] = {"rom_stem", r->stems[0], "ext", ext ? ext : "", "rom_md5", r->md5, "serial", r->serial,
                       "rom_fnv64", r->fnv64, "rom_stem_safe", r->safe_stem, "game_id", r->game_id,
                       "crc", r->crc[0] ? r->crc : "{crc}", NULL};
    memcpy(vars, v, sizeof v);
}

/* The first of a profile's names ("{serial}_1{ext},{rom_stem}_1{ext}") this
 * game has every value for, or -1. One already in `dir` (when given) comes
 * first: the emulator keeps using the file it has. */
static int first_known_name(const char *tmpls, const char *const vars[17], const char *dir, char *out, size_t n) {
    for (int pass = dir ? 0 : 1; pass < 2; pass++) {
        char list[256], *save = NULL;
        str_copy(list, sizeof list, tmpls);
        for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            str_template(out, n, t, vars);
            if (!out[0] || out[0] == '_' || out[0] == '.' || strchr(out, '{')) continue;
            if (pass == 1) return 0;
            char path[PATH_MAX_LEN], lower[256];
            path_join(path, sizeof path, dir, out);
            if (file_exists(path)) return 0;
            /* or in lower case, as SwanStationPS5 also reads it */
            str_copy(lower, sizeof lower, out);
            for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
            path_join(path, sizeof path, dir, lower);
            if (file_exists(path)) {
                str_copy(out, n, lower);
                return 0;
            }
        }
    }
    out[0] = 0;
    return -1;
}

/* Works out what the profile's extra names need (cached per file in the
 * state's roms entry, keyed by size and mtime), then adds those names. */
static void add_derived_stems(local_rom *r, cJSON *e) {
    const profile_map *m = r->map;
    const int nbase = r->nstems; /* the game's own names, before any made from them */
    size_t k = 0;
    for (const char *p = r->stems[0]; *p && k + 1 < sizeof r->safe_stem; p++)
        r->safe_stem[k++] = isalnum((unsigned char)*p) ? *p : '_';
    r->safe_stem[k] = 0;
    char file[PATH_MAX_LEN];
    int need_serial = map_uses(m, "{serial}"), need_fnv = map_uses(m, "{rom_fnv64}"), need_crc = map_uses(m, "{crc}");
    const int need_n64 = m->n64_split_dir[0] != 0;
    if ((need_serial || need_fnv || need_crc || need_n64) && game_file(r, file, sizeof file) == 0) {
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
        if (need_n64) {
            if (fresh && cJSON_GetObjectItemCaseSensitive(e, "n64_md5"))
                str_copy(r->n64_md5, sizeof r->n64_md5, jget_str(e, "n64_md5", ""));
            else if (n64_rom_md5(file, r->n64_md5) != 0)
                r->n64_md5[0] = 0;
        }
        if (need_crc) {
            uint32_t crc;
            if (fresh && cJSON_GetObjectItemCaseSensitive(e, "crc"))
                str_copy(r->crc, sizeof r->crc, jget_str(e, "crc", ""));
            else if (gameid_ps2_crc(file, &crc) == 0)
                snprintf(r->crc, sizeof r->crc, "%08X", crc);
            else
                r->crc[0] = 0;
        }
        if (e) {
            jset_num(e, "id_size", size);
            jset_num(e, "id_mtime", mtime);
            if (need_serial) jset_str(e, "serial", r->serial);
            if (need_fnv) jset_str(e, "fnv64", r->fnv64);
            if (need_crc) jset_str(e, "crc", r->crc);
            if (need_n64) jset_str(e, "n64_md5", r->n64_md5);
        }
    }
    const char *vars[17];
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
    /* A game in a folder can be named after any file in it, not just the
     * folder: SwanStationPS5 names a card after the disc or .m3u it boots. */
    for (int s = 1; s < nbase; s++) {
        char list[256], *save = NULL;
        str_copy(list, sizeof list, m->save_stems);
        for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            if (!strstr(t, "{rom_stem}")) continue;
            vars[1] = r->stems[s]; /* {rom_stem}'s value */
            char stem[256];
            str_template(stem, sizeof stem, t, vars);
            if (stem[0] && !strchr(stem, '{') && stem[0] != '_') add_stem(r, stem);
        }
    }
    vars[1] = r->stems[0];
    /* Names other cores of the emulator give the save ("core=template;..."). */
    char list[256], *save = NULL;
    str_copy(list, sizeof list, m->save_names_by_core);
    for (char *t = strtok_r(list, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(t, '='), stem[256];
        if (!eq) continue;
        str_template(stem, sizeof stem, eq + 1, vars);
        if (stem[0] && !strchr(stem, '{') && stem[0] != '_') add_stem(r, stem);
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

/* Whether a comma separated list has item. */
static int list_has(const char *list, const char *item) {
    size_t n = strlen(item);
    for (const char *p = list; n && (p = strstr(p, item)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || !p[n])) return 1;
    return 0;
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
    cJSON_DeleteItemFromObjectCaseSensitive(e, "miss_dirs");
}

/* Renames files in dir (and its folders one level down, RetroArch's per-core
 * ones) named "<old>.<ext>" to "<new>.<ext>", unless that's taken. */
static void rename_by_stem(const char *dir, const char *old_stem, const char *new_stem, int depth) {
    DIR *d = opendir(dir);
    struct dirent *de;
    size_t ol = strlen(old_stem);
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char from[PATH_MAX_LEN], to[PATH_MAX_LEN + 256];
        path_join(from, sizeof from, dir, de->d_name);
        if (dir_exists(from)) {
            if (depth == 0) rename_by_stem(from, old_stem, new_stem, 1);
            continue;
        }
        if (strncmp(de->d_name, old_stem, ol) != 0 || de->d_name[ol] != '.') continue;
        snprintf(to, sizeof to, "%s/%s%s", dir, new_stem, de->d_name + ol);
        if (file_exists(to) || rename(from, to) != 0) continue;
        LOGI("renamed %s -> %s", from, to);
        state_lock();
        static const char *const sections[] = {"saves", "states"};
        for (int i = 0; i < 2; i++) {
            cJSON *sec = state_section(sections[i]), *e = cJSON_DetachItemFromObjectCaseSensitive(sec, from);
            if (e) cJSON_AddItemToObject(sec, to, e);
        }
        state_unlock();
    }
    if (d) closedir(d);
}

/* Version 1.1.1 downloaded a game RomM keeps as a folder holding one file as
 * a single file named after the folder, with no extension, which emulators
 * don't list. It becomes that folder with the file inside under its own
 * name, as downloads do now; its saves and states take the file's name, which
 * is what the emulator looks for. Returns 1 when it moved the game. */
static int fix_bare_rom(const profile_map *m, const char *path, int rom_id) {
    char api[64];
    long st = 0;
    snprintf(api, sizeof api, "/api/roms/%d", rom_id);
    cJSON *rom = romm_call("GET", api, NULL, &st);
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files");
    const char *fn = path_basename(jget_str(cJSON_GetArrayItem(files, 0), "file_name", ""));
    int done = 0;
    if (st == 200 && cJSON_GetArraySize(files) == 1 && path_ext(fn)[0] &&
        !strcmp(jget_str(rom, "fs_name", ""), path_basename(path))) {
        char tmp[PATH_MAX_LEN + 8], inside[PATH_MAX_LEN + 256];
        snprintf(tmp, sizeof tmp, "%s.moving", path);
        snprintf(inside, sizeof inside, "%s/%s", path, fn);
        if (rename(path, tmp) == 0) {
            if (mkdir_p(path) == 0 && rename(tmp, inside) == 0) {
                char old_stem[256], new_stem[256];
                str_copy(old_stem, sizeof old_stem, path_basename(path));
                path_stem(new_stem, sizeof new_stem, fn);
                LOGI("moved %s into %s, so the emulator lists it", path, inside);
                state_lock();
                cJSON *e = state_entry("roms", path);
                if (e) {
                    cJSON_DeleteItemFromObjectCaseSensitive(e, "stems");
                    cJSON *stems = cJSON_AddArrayToObject(e, "stems");
                    cJSON_AddItemToArray(stems, cJSON_CreateString(old_stem));
                    cJSON_AddItemToArray(stems, cJSON_CreateString(new_stem));
                }
                state_unlock();
                if (strcmp(old_stem, new_stem) != 0) {
                    rename_by_stem(m->save_dir, old_stem, new_stem, 0);
                    if (strcmp(m->state_dir, m->save_dir) != 0) rename_by_stem(m->state_dir, old_stem, new_stem, 0);
                }
                done = 1;
            } else {
                rmdir(path);
                rename(tmp, path); /* put it back as it was */
            }
        }
    }
    cJSON_Delete(rom);
    return done;
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

        /* Several platforms of one emulator can share a folder. Other
         * emulators playing the same folder (an extra with a mount, or one
         * set to the main one's games) each have the game, for their saves. */
        int dup = 0;
        for (int i = 0; i < c->nroms; i++)
            if (!strcmp(c->roms[i].path, path) && !strcmp(c->roms[i].map->profile_id, m->profile_id) &&
                !strcmp(c->roms[i].map->emulator, m->emulator)) /* (a core twin has its own) */
                dup = 1;
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
            char only[256] = "";
            int files = 0;
            while (sd && (se = readdir(sd))) {
                if (se->d_name[0] == '.') continue;
                path_stem(stem, sizeof stem, se->d_name);
                if (ext_in_list(se->d_name, ".cue,.m3u,.gdi,.chd,.ccd,.iso,.pbp")) add_stem(&r, stem);
                if (files++ == 0) str_copy(only, sizeof only, stem);
            }
            if (sd) closedir(sd);
            /* One file in the folder (RomM keeps some single-file games so):
             * emulators name its saves after that file. */
            if (files == 1) {
                /* The emulator names saves after the file inside, so new
                 * saves take its name (the folder's still matches old ones). */
                add_stem(&r, only);
                for (int k = 1; k < r.nstems; k++)
                    if (!strcmp(r.stems[k], only)) {
                        char first[sizeof r.stems[0]];
                        str_copy(first, sizeof first, r.stems[0]);
                        str_copy(r.stems[0], sizeof r.stems[0], only);
                        str_copy(r.stems[k], sizeof r.stems[k], first);
                    }
            }
        } else {
            path_stem(stem, sizeof stem, de->d_name);
            add_stem(&r, stem);
        }

        state_lock();
        cJSON *e = state_entry("roms", path);
        int rom_id = e ? (int)jget_num(e, "rom_id", 0) : 0;
        time_t miss_at = e ? (time_t)jget_num(e, "miss_at", 0) : 0;
        /* A folder that holds several of the emulator's systems (Porpoise:
         * GameCube and Wii): a game is the system's it matched for, and a
         * miss for one system doesn't stop the others asking. */
        const char *was_dir = e ? jget_str(e, "platform_dir", "") : "";
        const int other_system = e && rom_id && was_dir[0] && !strcmp(jget_str(e, "profile", ""), m->profile_id) &&
                                 strcmp(was_dir, m->platform_dir) != 0;
        if (miss_at && !list_has(jget_str(e, "miss_dirs", ""), m->platform_dir)) miss_at = 0;
        if (other_system) {
            state_unlock();
            continue;
        }
        /* Seen under this emulator now (an older version named its profile otherwise). */
        if (e && rom_id && strcmp(jget_str(e, "profile", ""), m->profile_id) != 0) {
            int stale = 1; /* no profile has that name now */
            for (int k = 0; k < c->nmaps && stale; k++) stale = strcmp(c->maps[k].profile_id, jget_str(e, "profile", "")) != 0;
            /* In a folder shared by systems, the system it was recorded for takes it. */
            int theirs = 0;
            for (int k = 0; k < c->nmaps && stale && was_dir[0] && strcmp(was_dir, m->platform_dir) != 0; k++)
                theirs |= !strcmp(c->maps[k].profile_id, m->profile_id) && !strcmp(c->maps[k].platform_dir, was_dir);
            if (theirs) {
                state_unlock();
                continue;
            }
            if (stale) {
                jset_str(e, "profile", m->profile_id);
                jset_str(e, "platform_dir", m->platform_dir);
            }
        }
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
                char dirs[256];
                const char *had = jget_str(ne, "miss_dirs", "");
                if (list_has(had, m->platform_dir)) str_copy(dirs, sizeof dirs, had);
                else snprintf(dirs, sizeof dirs, "%s%s%s", had, had[0] ? "," : "", m->platform_dir);
                jset_num(ne, "rom_id", 0);
                jset_num(ne, "miss_at", (double)now);
                jset_str(ne, "miss_dirs", dirs);
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
        if (!is_dir && !path_ext(de->d_name)[0] && fix_bare_rom(m, path, rom_id)) {
            char fixed[PATH_MAX_LEN];
            str_copy(fixed, sizeof fixed, path); /* the folder, now */
            DIR *sd = opendir(fixed);
            struct dirent *se;
            while (sd && (se = readdir(sd)))
                if (se->d_name[0] != '.') {
                    path_stem(stem, sizeof stem, se->d_name);
                    add_stem(&r, stem);
                }
            if (sd) closedir(sd);
        }
        r.rom_id = rom_id;
        if (map_uses(m, "{game_id}") && rom_game_id(&r, r.game_id, sizeof r.game_id) != 0) r.game_id[0] = 0;
        if (m->save_stems[0] || m->state_stems[0] || map_uses(m, "{rom_stem_safe}") || m->n64_split_dir[0]) {
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
    /* A folder per game, by its id (Porpoise's states/GAFE01) or its file's
     * name (Seedee5's nvram/<game>/cdimono1). */
    const char *at = strstr(states ? ru->state_dir : ru->save_dir, "{game_id}");
    if (at) {
        char id[24];
        if (rom_game_id(r, id, sizeof id) != 0) return 0;
        const char *dir = states ? ru->state_dir : ru->save_dir;
        snprintf(out, n, "%.*s%s%s", (int)(at - dir), dir, id, at + 9);
        return 1;
    }
    const char *dir = states ? ru->state_dir : ru->save_dir, *stem = strstr(dir, "{rom_stem}");
    if (stem) {
        snprintf(out, n, "%.*s%s%s", (int)(stem - dir), dir, r->stems[0], stem + 10);
        return 1;
    }
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
        if (!r->rules.sync_saves || (states && !r->map->sync_states) || (!states && r->map->save_layout[0])) continue;
        if (!ext_in_list(file_name, states ? r->map->state_exts : r->map->save_exts)) continue;
        const char *dir = states ? r->rules.state_dir : r->rules.save_dir;
        if (strstr(dir, "{game_id}") || strstr(dir, "{rom_stem}")) {
            char exp[PATH_MAX_LEN];
            if (expected_dir(r, states, exp, sizeof exp) && !strcmp(exp, file_dir)) {
                *active = 1;
                return r;
            }
            continue;
        }
        for (int s = 0; s < r->nstems; s++) {
            size_t l = strlen(r->stems[s]);
            /* In any case: SwanStationPS5 takes "slus-01272_1.mcd" for SLUS-01272. */
            if (strncasecmp(file_name, r->stems[s], l) != 0 || file_name[l] != '.') continue;
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

/* DeSmuME's .dsv: the DS cartridge save, then 122 bytes of its own: the snip
 * line below, six numbers (size, padded size, save type, address size,
 * memory size, version) and the cookie. Everyone else (melonDS, PS5-ECAM,
 * RomM's player) keeps the save alone, so the save is what's compared, sent
 * and shared, and the footer is DeSmuME's business. */
#define DSV_SNIP "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:"
#define DSV_COOKIE "|-DESMUME SAVE-|"
#define DSV_FOOTER (sizeof DSV_SNIP - 1 + 24 + sizeof DSV_COOKIE - 1)

static size_t dsv_save_len(const char *b, size_t n) {
    if (n < DSV_FOOTER || memcmp(b + n - (sizeof DSV_COOKIE - 1), DSV_COOKIE, sizeof DSV_COOKIE - 1) != 0) return n;
    return n - DSV_FOOTER;
}

/* A save's hash: a .dsv's without its footer. */
static int save_md5(const char *path, char out[33]) {
    char tail[sizeof DSV_COOKIE - 1];
    const int64_t n = file_size(path);
    FILE *fp = n >= (int64_t)DSV_FOOTER ? fopen(path, "rb") : NULL;
    const int dsv = fp && fseek(fp, -(long)sizeof tail, SEEK_END) == 0 && fread(tail, 1, sizeof tail, fp) == sizeof tail &&
                    !memcmp(tail, DSV_COOKIE, sizeof tail);
    if (fp) fclose(fp);
    if (!dsv) return md5_file_hex(path, out);
    size_t len = 0;
    char *b = read_file(path, &len);
    if (!b) return -1;
    md5_hex(b, dsv_save_len(b, len), out);
    free(b);
    return 0;
}

/* DeSmuME's type for a save of n bytes (its save_types table), and how many
 * address bytes the chip takes. */
static void dsv_type(size_t n, uint32_t *type, uint32_t *addr) {
    static const uint32_t sizes[] = {0, 512, 8192, 65536, 32768, 262144, 524288, 1048576, 2097152, 4194304,
                                     8388608, 16777216, 33554432, 67108864};
    *type = 0;
    for (uint32_t i = 1; i < sizeof sizes / sizeof *sizes; i++)
        if (sizes[i] == n) *type = i;
    *addr = n <= 512 ? 1 : n <= 65536 ? 2 : 3;
}

/* A save made ready for an emulator keeping it in format: footer off (one
 * DeSmuME sent), and DeSmuME's put on for "nds-dsv": the one dest has when
 * the size matches, else one made for the size. In place; 0 when done or
 * nothing needed doing. */
static int fit_save(const char *format, const char *path, const char *dest) {
    if (strcmp(profiles_save_family(format), "nds-raw") != 0) return 0;
    size_t len = 0;
    char *b = read_file(path, &len);
    if (!b) return -1;
    const size_t save = dsv_save_len(b, len);
    if (strcmp(format, "nds-dsv") != 0) {
        const int rc = save == len ? 0 : write_file_atomic(path, b, save);
        free(b);
        return rc;
    }
    char footer[DSV_FOOTER];
    size_t had = 0;
    char *old = dest ? read_file(dest, &had) : NULL;
    if (old && dsv_save_len(old, had) == save && had == save + DSV_FOOTER) {
        memcpy(footer, old + save, DSV_FOOTER);
    } else {
        unsigned char nums[24];
        uint32_t type, addr;
        dsv_type(save, &type, &addr);
        const uint32_t v[6] = {(uint32_t)save, (uint32_t)save, type, addr, (uint32_t)save, 0};
        for (int i = 0; i < 6; i++)
            for (int k = 0; k < 4; k++) nums[i * 4 + k] = (unsigned char)(v[i] >> (8 * k));
        memcpy(footer, DSV_SNIP, sizeof DSV_SNIP - 1);
        memcpy(footer + sizeof DSV_SNIP - 1, nums, sizeof nums);
        memcpy(footer + sizeof DSV_SNIP - 1 + sizeof nums, DSV_COOKIE, sizeof DSV_COOKIE - 1);
    }
    free(old);
    char *out = malloc(save + DSV_FOOTER);
    int rc = -1;
    if (out) {
        memcpy(out, b, save);
        memcpy(out + save, footer, DSV_FOOTER);
        rc = write_file_atomic(path, out, save + DSV_FOOTER);
    }
    free(out);
    free(b);
    return rc;
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
    if (!f->hash[0]) save_md5(f->path, f->hash);
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

/* Saves on a PS1 memory card (directory frames starting one), or -1 when
 * it isn't one. */
static int psx_card_saves(const profile_map *m, const char *path) {
    if (strcmp(m->save_format, "psx-card") != 0) return -1;
    unsigned char dir[16 * 128];
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    size_t n = fread(dir, 1, sizeof dir, fp);
    fclose(fp);
    if (n < sizeof dir || dir[0] != 'M' || dir[1] != 'C') return -1;
    int used = 0;
    for (int i = 1; i < 16; i++) used += dir[i * 128] == 0x51;
    return used;
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
                    /* Said once per pair while running, not at every sync. */
                    static cJSON *told;
                    char key[2 * PATH_MAX_LEN + 8];
                    snprintf(key, sizeof key, "%s|%s", o->path, f.path);
                    state_lock();
                    if (!told) told = cJSON_CreateObject();
                    const int first = !cJSON_GetObjectItemCaseSensitive(told, key);
                    if (first) cJSON_AddTrueToObject(told, key);
                    state_unlock();
                    if (first)
                        LOGW("two local saves for rom %d slot %s: %s, %s (using newest)", f.rom_id, f.slot, o->path, f.path);
                    /* A blank card (an emulator made a fresh one under another
                     * name) never beats one with saves on it. */
                    int fo = psx_card_saves(r->map, f.path), oo = psx_card_saves(r->map, o->path);
                    int take = fo == 0 && oo > 0 ? 0 : oo == 0 && fo > 0 ? 1 : f.mtime > o->mtime;
                    if (take) {
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
        /* A folder per game ("states/{game_id}", "nvram/{rom_stem}/cdimono1"):
         * scan from its parent. */
        char *id = strstr(out, "/{game_id}");
        if (!id) id = strstr(out, "/{rom_stem}");
        if (id) *id = 0;
    }
}

static void scan_saves(scan_ctx *c) {
    state_lock();
    cJSON_Delete(g_strays);
    g_strays = cJSON_CreateArray();
    cJSON_Delete(g_roots_next);
    g_roots_next = cJSON_CreateArray();
    cJSON_Delete(g_bundle_roots_next);
    g_bundle_roots_next = cJSON_CreateArray();
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
            cJSON_AddItemToArray(g_roots_next, cJSON_CreateString(dir));
            state_unlock();
        }
    }
    scan_bundles(c);
    state_lock();
    cJSON_Delete(g_roots);
    g_roots = g_roots_next;
    cJSON_Delete(g_bundle_roots);
    g_bundle_roots = g_bundle_roots_next;
    g_roots_next = g_bundle_roots_next = NULL;
    state_unlock();
}


static int scan_prepare(scan_ctx *c) {
    memset(c, 0, sizeof *c);
    g_nfresh = 0;
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
    c->server_status = st;
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
    cJSON_Delete(c->romm_saves);
    cJSON_Delete(c->romm_has);
    profiles_free(c->maps);
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

/* Whether a save here (a bundle's "<dir>#<id>") was ever synced with RomM. */
static int ever_synced(const char *path) {
    state_lock();
    const cJSON *e = state_entry("saves", path);
    const int known = e && jget_num(e, "synced_at", 0) > 0;
    state_unlock();
    return known;
}

/* Whether a save was made while its emulator ran, in the last run RommPS saw
 * of it (main.c reports each): someone played and saved, it's no leftover. */
static int saved_in_last_run(const profile_map *m, time_t mtime) {
    if (!m || !m->app_title[0]) return 0;
    state_lock();
    const cJSON *e = state_entry("runs", m->app_title);
    const time_t start = e ? (time_t)jget_num(e, "start", 0) : 0, end = e ? (time_t)jget_num(e, "end", 0) : 0;
    state_unlock();
    /* Some write the card a moment after the game says it closed. */
    return start > 0 && mtime >= start - 5 && mtime <= end + 120;
}

/* Whether a copy of a game's saves counts against another emulator's: one
 * RomM knows does, and so does one saved in its emulator's last run. Not one
 * an emulator just turned on came with, saved before RommPS ever saw it
 * (Brilliant Diamond: PS5CEMU-HAR's old save, opened last, would have
 * replaced ProsperoEden's on RomM and in ProsperoEden). */
static int counts(const char *path, const profile_map *m, time_t mtime) {
    return ever_synced(path) || saved_in_last_run(m, mtime);
}

/* Of two copies of one game's saves in two emulators, whether a beats b: one
 * that counts beats one that doesn't, else the one saved last. */
static int beats(int a_counts, time_t a_mtime, int b_counts, time_t b_mtime) {
    if (a_counts != b_counts) return a_counts;
    return a_mtime > b_mtime;
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
    state_entry_remove("too_big", f->path);
    state_entry_remove("conflicts", f->path);
    state_entry_remove("unplaced", f->path);
    state_unlock();
}

static const local_file *find_save(const scan_ctx *c, int rom_id, const char *slot);

#define Z32 "00000000000000000000000000000000" /* Azahar's SD card and console ids */

/* ---- PS2 memory cards (pcsx2_cards in profiles.h) ---- */

/* The disc a PS2 game's CRC is read from: the file, or a folder game's first image. */
static void ps2_disc(const local_rom *r, char *out, size_t n) {
    str_copy(out, n, r->path);
    if (!dir_exists(r->path)) return;
    DIR *d = opendir(r->path);
    struct dirent *de;
    out[0] = 0;
    while (d && (de = readdir(d)) && !out[0])
        if (ext_in_list(de->d_name, ".iso,.chd,.cue,.bin")) path_join(out, n, r->path, de->d_name);
    if (d) closedir(d);
}

/* The name the emulator knows the game by ({rom_stem}): its file's, or for a
 * folder game the first image's, which is the one it starts. */
static void ps2_stem(const local_rom *r, char *out, size_t n) {
    char disc[PATH_MAX_LEN];
    ps2_disc(r, disc, sizeof disc);
    path_stem(out, n, disc[0] ? disc : r->path);
}

/* PCSX2's CRC of the game ("248E6126"), kept with the game while its file
 * stays the same. 0 when known. */
static int ps2_crc(const local_rom *r, char out[9]) {
    char disc[PATH_MAX_LEN], key[64];
    ps2_disc(r, disc, sizeof disc);
    struct stat st;
    if (!disc[0] || stat(disc, &st) != 0) return -1;
    snprintf(key, sizeof key, "%lld:%lld", (long long)st.st_mtime, (long long)st.st_size);
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    int have = e && !strcmp(jget_str(e, "ps2_crc_key", ""), key) && strlen(jget_str(e, "ps2_crc", "")) == 8;
    if (have) str_copy(out, 9, jget_str(e, "ps2_crc", ""));
    state_unlock();
    if (have) return 0;
    uint32_t crc;
    if (gameid_ps2_crc(disc, &crc) != 0) return -1;
    snprintf(out, 9, "%08X", crc);
    state_lock();
    if ((e = state_entry("roms", r->path))) {
        jset_str(e, "ps2_crc", out);
        jset_str(e, "ps2_crc_key", key);
    }
    state_unlock();
    return 0;
}

/* The game's own settings file, its names filled in; 0 when it has one. */
static int ps2_game_ini(const local_rom *r, char *out, size_t n) {
    const char *tpl = r->map->pc_game_ini;
    char stem[256], serial[24] = "", crc[9] = "";
    if (!tpl[0]) return -1;
    ps2_stem(r, stem, sizeof stem);
    if (strstr(tpl, "{serial}") && rom_game_id(r, serial, sizeof serial) != 0) return -1;
    if (strstr(tpl, "{crc}") && ps2_crc(r, crc) != 0) return -1;
    const char *vars[] = {"rom_stem", stem, "serial", serial, "crc", crc, NULL};
    str_template(out, n, tpl, vars);
    return 0;
}

/* The memory card settings in one file: later files win, as the emulator
 * applies them (PS5SX2: gs.ini, then the game's own). */
static void ps2_slot_settings(const char *path, int ini, ps2_cards *out, int on[2]) {
    char *txt = path[0] ? read_file(path, NULL) : NULL;
    for (int s = 0; txt && s < 2; s++) {
        char key[32], v[256];
        snprintf(key, sizeof key, "Slot%d_Filename", s + 1);
        if (ps2card_setting(txt, ini, key, v, sizeof v) == 0 && v[0]) str_copy(out->slot[s], sizeof out->slot[s], v);
        snprintf(key, sizeof key, "Slot%d_Enable", s + 1);
        if (ps2card_setting(txt, ini, key, v, sizeof v) == 0) on[s] = strcasecmp(v, "false") != 0 && strcmp(v, "0") != 0;
    }
    free(txt);
}

/* The card folder and the cards in the slots: PCSX2's Mcd001.ps2 and
 * Mcd002.ps2, unless the emulator's settings or the game's own name others
 * ("" for a slot with no card in it). */
static void ps2_cards_for(const local_rom *r, ps2_cards *out) {
    memset(out, 0, sizeof *out);
    profile_card_dir(r->map, out->dir, sizeof out->dir);
    if (!dir_exists(out->dir)) out->dir[0] = 0;
    str_copy(out->slot[0], sizeof out->slot[0], "Mcd001.ps2");
    str_copy(out->slot[1], sizeof out->slot[1], "Mcd002.ps2");
    const int ini = !strcmp(r->map->pc_format, "ini");
    int on[2] = {1, 1};
    char game[PATH_MAX_LEN];
    ps2_slot_settings(r->map->pc_global_ini, ini, out, on);
    const int global_slot1 = on[0];
    int game_slot1 = 0; /* the game's settings say anything of slot 1 */
    if (ps2_game_ini(r, game, sizeof game) == 0) {
        ps2_slot_settings(game, ini, out, on);
        char *txt = read_file(game, NULL), v[256];
        game_slot1 = txt && (ps2card_setting(txt, ini, "Slot1_Filename", v, sizeof v) == 0 ||
                             ps2card_setting(txt, ini, "Slot1_Enable", v, sizeof v) == 0);
        free(txt);
    }
    /* The emulator's own card per game (Crux2): slot 1 holds it unless the
     * game's settings say anything of slot 1 or it's off for every game. */
    if (r->map->pc_auto_card[0] && global_slot1 && !game_slot1) {
        char *txt = read_file(r->map->pc_global_ini, NULL), v[16] = "", serial[24], card[64];
        const int auto_on = !txt || ps2card_setting(txt, 1, r->map->pc_auto_card, v, sizeof v) != 0 ||
                            (strcasecmp(v, "false") != 0 && strcmp(v, "0") != 0);
        free(txt);
        if (auto_on && rom_game_id(r, serial, sizeof serial) == 0 &&
            ps2card_db_card(r->map->pc_game_db, serial, card, sizeof card) == 0)
            str_copy(out->slot[0], sizeof out->slot[0], card);
    }
    for (int s = 0; s < 2; s++)
        if (!on[s]) out->slot[s][0] = 0;
}

/* Whether the emulator is closed, so its cards, settings or hard disk can
 * change: none of its processes runs (a console resting with it open still
 * has them). */
static int emulator_closed(const profile_map *m) {
    const int alive = m->app_title[0] ? plat_app_alive(m->app_title) : -1;
    return alive == 0 || (alive < 0 && !g_game_running);
}

/* One card per game: a game whose settings name no card for slot 1 gets its
 * own ("Tekken 5 (USA).ps2"), with its saves from the card it played on so
 * far copied onto it; that card stays as it was. Slot 2 is left alone, so
 * games that look for another game's saves still find those. */
static void ps2_own_card(const local_rom *r, const char *serial) {
    const profile_map *m = r->map;
    if (strcmp(m->save_layout, "ps2") != 0 || !m->pc_own_card[0] || !m->cards_active || strcmp(m->cards_mode, "per_game") != 0)
        return;
    const int ini = !strcmp(m->pc_format, "ini");
    char path[PATH_MAX_LEN], v[256], stem[256], own[256];
    if (ps2_game_ini(r, path, sizeof path) != 0) return;
    char *txt = read_file(path, NULL);
    const int named = txt && ps2card_setting(txt, ini, "Slot1_Filename", v, sizeof v) == 0 && v[0];
    free(txt);
    if (named || !emulator_closed(m)) return;
    ps2_cards now;
    ps2_cards_for(r, &now);
    if (!now.dir[0]) return;
    ps2_stem(r, stem, sizeof stem);
    const char *vars[] = {"rom_stem", stem, NULL};
    str_template(own, sizeof own, m->pc_own_card, vars);
    if (!own[0] || strchr(own, '/') || !strcmp(own, now.slot[1])) return;
    char card[PATH_MAX_LEN];
    path_join(card, sizeof card, now.dir, own);
    if (!file_exists(card) && !dir_exists(card)) {
        /* Made now, formatted: the game finds it ready, and what comes from
         * RomM lands on it, never on the shared card in slot 2. */
        if (ps2card_blank(card) != 0) {
            LOGW("%s: can't make its memory card, %s", path_basename(r->path), card);
            return;
        }
        ps2_cards from = now, to = now;
        from.slot[1][0] = to.slot[1][0] = 0;
        str_copy(to.slot[0], sizeof to.slot[0], own);
        unsigned char *unit = NULL;
        size_t len = 0;
        char hash[33], name[256], identity[33];
        int rc = from.slot[0][0] ? ps2card_collect(&from, r->path, serial, &unit, &len, hash, name, identity) : 0;
        const int had = unit != NULL;
        if (rc == 0 && had) rc = ps2card_restore(&to, r->path, serial, unit, len);
        free(unit);
        if (rc != 0) {
            unlink(card);
            return;
        }
        if (had) LOGI("%s: its saves on %s copied to its own card, %s", path_basename(r->path), from.slot[0], own);
    }
    if (ps2card_set_setting(path, ini, "Slot1_Filename", own) != 0) {
        LOGW("%s: can't write %s", path_basename(r->path), path);
        return;
    }
    LOGI("%s: plays on its own memory card, %s (%s)", path_basename(r->path), own, path);
}

/* Candidate folders for a game's bundled saves. */
static int bundle_bases(const local_rom *r, char bases[][PATH_MAX_LEN], int max) {
    if (!strcmp(r->map->save_layout, "ps2")) { /* the slot cards themselves */
        ps2_cards pc;
        ps2_cards_for(r, &pc);
        int n = 0;
        for (int s = 0; s < 2 && n < max && pc.dir[0]; s++)
            if (pc.slot[s][0] && (s == 0 || strcmp(pc.slot[1], pc.slot[0]) != 0)) path_join(bases[n++], PATH_MAX_LEN, pc.dir, pc.slot[s]);
        return n;
    }
    if (!strcmp(r->map->save_layout, "xbox")) { /* the hard disk image, wherever xemu.toml says */
        char toml[PATH_MAX_LEN];
        path_join(toml, sizeof toml, r->map->root, "xemu.toml");
        char *t = read_file(toml, NULL), *at = t ? strstr(t, "hdd_path") : NULL;
        char *q = at ? strpbrk(at, "'\"\n") : NULL;
        char *end = q && *q != '\n' ? strchr(q + 1, *q) : NULL;
        if (end && end - q - 1 > 0 && (size_t)(end - q) < PATH_MAX_LEN) {
            memcpy(bases[0], q + 1, (size_t)(end - q - 1));
            bases[0][end - q - 1] = 0;
        } else {
            path_join(bases[0], PATH_MAX_LEN, r->map->root, "xbox_hdd.qcow2");
        }
        free(t);
        return max > 0;
    }
    static const char *defaults[][3] = {
        /* {save_path} follows RetroArch's sorting. {save_root} is the bare save
         * folder, because Dolphin ignores sort-by-core entirely and drops
         * dolphin-emu/User right in savefiles/. Thanks, Dolphin. */
        {"psp", "{save_path}/PSP/SAVEDATA,{save_root}/PPSSPP/PSP/SAVEDATA,{bios}/PPSSPP/PSP/SAVEDATA", NULL},
        {"gci", "{save_root}/dolphin-emu/User/GC,{save_path}/User/GC,{save_path}/dolphin-emu/User/GC,{bios}/dolphin-emu/User/GC", NULL},
        {"wii", "{save_root}/dolphin-emu/User/Wii,{save_path}/User/Wii,{save_path}/dolphin-emu/User/Wii,{bios}/dolphin-emu/User/Wii", NULL},
        /* RetroArch's Azahar keeps its user folder in savefiles/Azahar, or
         * in system/Azahar without a save folder. */
        {"3ds", "{save_root}/Azahar/sdmc/Nintendo 3DS/" Z32 "/" Z32 "/title,{bios}/Azahar/sdmc/Nintendo 3DS/" Z32 "/" Z32 "/title", NULL},
    };
    const char *tpl = r->map->bundle_dirs[0] ? r->map->bundle_dirs : NULL;
    for (size_t i = 0; !tpl && i < sizeof defaults / sizeof defaults[0]; i++)
        if (!strcmp(defaults[i][0], r->map->save_layout)) tpl = defaults[i][1];
    if (!tpl) return 0;
    char save_path[PATH_MAX_LEN], list[2048], *save = NULL;
    if (!expected_dir(r, 0, save_path, sizeof save_path)) str_copy(save_path, sizeof save_path, r->rules.save_dir);
    const char *vars[] = {"save_path", save_path, "save_root", r->rules.save_dir, "bios", r->map->bios_dir, "root", r->map->root,
                          "switch_user", r->map->switch_user, "xuid", r->map->xuid, NULL};
    str_template(list, sizeof list, tpl, vars);
    int n = 0;
    for (char *t = strtok_r(list, ",", &save); t && n < max; t = strtok_r(NULL, ",", &save))
        str_copy(bases[n++], PATH_MAX_LEN, t);
    return n;
}

/* An Xbox disk skipped because xemu still ran, during this sync. */
static int g_disk_busy;

/* psp, ps3: the other title ids a game's saves came down from RomM under
 * (bundle.also). One that's another game's own id here stays that game's:
 * with both releases around, the original keeps its folders. */
static void save_ids_of(const char *game_id, char *out, size_t n) {
    char list[64], *save = NULL;
    size_t o = 0;
    out[0] = 0;
    state_lock();
    str_copy(list, sizeof list, jget_str(state_entry("save_ids", game_id), "also", ""));
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int owned = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, state_section("roms")) owned |= !strcasecmp(jget_str(e, "game_id", ""), t);
        if (!owned && o + strlen(t) + 2 <= n) o += (size_t)snprintf(out + o, n - o, "%s%s", o ? "," : "", t);
    }
    state_unlock();
}

static void save_ids_learn(const char *game_id, const char *ids) {
    char all[64], list[64], *save = NULL;
    state_lock();
    cJSON *e = state_entry_ensure("save_ids", game_id);
    str_copy(all, sizeof all, jget_str(e, "also", ""));
    str_copy(list, sizeof list, ids);
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        const size_t o = strlen(all);
        if (strstr(all, t) || o + strlen(t) + 2 > sizeof all) continue;
        snprintf(all + o, sizeof all - o, "%s%s", o ? "," : "", t);
        LOGI("%s: its saves also go by %s", game_id, t);
    }
    jset_str(e, "also", all);
    state_unlock();
}

static int bundle_for_rom(const local_rom *r, const char *game_id, bundle *b) {
    char bases[6][PATH_MAX_LEN], also[64] = "";
    int n = bundle_bases(r, bases, 6);
    /* xemu writes its disk as it goes: while it runs, the copy made last. */
    if (n && !strcmp(r->map->save_layout, "xbox") && !emulator_closed(r->map)) {
        bases[0][0] = 0;
        g_disk_busy = 1;
    }
    if (!strcmp(r->map->save_layout, "psp") || !strcmp(r->map->save_layout, "ps3")) save_ids_of(game_id, also, sizeof also);
    return n ? bundle_open(b, r->map->save_layout, game_id, also, bases, n) : -1;
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

/* A Switch game's title id from its file's name, as dumps carry it
 * ("Pokemon Brilliant Diamond [0100000011D90000][v0].nsp"), or from a folder
 * game's files. An update's id names its game (the last three digits 000), as
 * the game's save folder does. */
static int switch_title(const char *path, char *out, size_t n) {
    const char *name = path_basename(path);
    for (const char *p = strchr(name, '['); p; p = strchr(p + 1, '[')) {
        size_t k = 0;
        while (k < 16 && isxdigit((unsigned char)p[1 + k])) k++;
        if (k != 16 || p[17] != ']') continue;
        char hex[17];
        memcpy(hex, p + 1, 16);
        hex[16] = 0;
        snprintf(out, n, "%016llX", strtoull(hex, NULL, 16) & ~0xFFFULL);
        return 0;
    }
    DIR *d = dir_exists(path) ? opendir(path) : NULL;
    struct dirent *de;
    int rc = -1;
    while (d && rc != 0 && (de = readdir(d)))
        if (de->d_name[0] != '.') rc = switch_title(de->d_name, out, n);
    if (d) closedir(d);
    return rc;
}

/* RomM no longer has the game this file was matched to (a rescan gives games
 * new ids): the match goes, and the next sync finds it again by name. */
static void forget_gone_rom(const local_rom *r) {
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    const int had = e && (int)jget_num(e, "rom_id", 0) == r->rom_id;
    if (had) {
        jset_num(e, "rom_id", 0);
        cJSON_DeleteItemFromObjectCaseSensitive(e, "game_id");
        cJSON_DeleteItemFromObjectCaseSensitive(e, "miss_at");
    }
    state_unlock();
    if (had) LOGI("%s: RomM no longer has game %d, looked for again by name", path_basename(r->path), r->rom_id);
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

    /* A front end that lists its games' ids (PSFlyCast), then the disc
     * ("disc_id": "dreamcast"). RomM has no title id for these, so it isn't asked. */
    if (r->map->game_ids[0] || !strcmp(r->map->disc_id, "dreamcast")) {
        char disc[PATH_MAX_LEN] = "";
        int ok = r->map->game_ids[0] && gameid_from_list(r->map->game_ids, r->map->root, r->path, out, n) == 0;
        if (!ok && dir_exists(r->path)) {
            DIR *d = opendir(r->path);
            struct dirent *de;
            while (d && (de = readdir(d)) && !disc[0])
                if (ext_in_list(de->d_name, ".gdi,.chd,.cue,.iso")) path_join(disc, sizeof disc, r->path, de->d_name);
            if (d) closedir(d);
        } else if (!ok) {
            str_copy(disc, sizeof disc, r->path);
        }
        if (!ok && (!disc[0] || gameid_dreamcast(disc, out, n) != 0)) {
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

    char disc[PATH_MAX_LEN];
    str_copy(disc, sizeof disc, r->path);
    if (dir_exists(r->path)) { /* multi-file game: use its first disc image */
        DIR *d = opendir(r->path);
        struct dirent *de;
        disc[0] = 0;
        while (d && (de = readdir(d)) && !disc[0])
            if (ext_in_list(de->d_name, ".iso,.cso,.rvz,.wia,.gcm,.ciso,.wbfs,.cci,.3ds,.cxi,.cia,.chd,.xiso")) path_join(disc, sizeof disc, r->path, de->d_name);
        if (d) closedir(d);
    }
    const char *layout = r->map->save_layout;
    const int title = !strcmp(layout, "3ds") || !strcmp(layout, "wiiu"), nx = !strcmp(layout, "switch"),
              x360 = !strcmp(layout, "x360"), xbox = x360 || !strcmp(layout, "xbox");
    int ok = 0;
    if (x360) { /* a GOD or STFS package has no XEX to read: RomM's save_target */
        ok = gameid_x360(r->path, out, n) == 0 || (disc[0] && gameid_x360(disc, out, n) == 0);
    } else if (xbox) {
        ok = gameid_xbox(r->path, out, n) == 0 || (disc[0] && gameid_xbox(disc, out, n) == 0);
    } else if (nx) {
        ok = switch_title(r->path, out, n) == 0;
    } else if (title) {
        /* The front end's list first (a .wux can't be read without its disc
         * key), then the game file's header. */
        char lib[PATH_MAX_LEN];
        path_join(lib, sizeof lib, r->map->root, "library.json");
        ok = gameid_from_library(lib, layout, r->path, out, n) == 0 ||
             (!strcmp(layout, "3ds") && disc[0] && gameid_3ds(disc, out, n) == 0);
    } else if (!strcmp(layout, "ps2")) {
        ok = disc[0] && gameid_psx(disc, out, n) == 0; /* SYSTEM.CNF's BOOT2: "SLUS-21059" */
    } else {
        ok = disc[0] && (!strcmp(layout, "psp") ? gameid_psp(disc, out, n) : gameid_dolphin(disc, out, n)) == 0;
    }
    if (!ok) {
        char api[64];
        long st = 0;
        snprintf(api, sizeof api, "/api/roms/%d", r->rom_id);
        cJSON *rom = romm_call("GET", api, NULL, &st);
        if (st == 404) forget_gone_rom(r);
        /* An Xbox or 360 game's save folder is RomM's save_target, "4D530004"
         * (an Xbox title id is "MS-004"). */
        const char *tid = jget_str(rom, xbox ? "save_target" : "title_id", "");
        size_t o = 0;
        for (; *tid && o + 1 < n; tid++) if (isalnum((unsigned char)*tid)) out[o++] = (char)toupper((unsigned char)*tid);
        out[o] = 0;
        ok = title || nx ? o == 16 : xbox ? o == 8 : o >= 4;
        /* A PS2 serial keeps its dash, as the saves on a card name it. */
        if (!strcmp(layout, "ps2") && o == 9 && o + 1 < n) {
            memmove(out + 5, out + 4, 6);
            out[4] = '-';
        }
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

/* ---- GameCube: the form RomM keeps a game's saves in ----
 * RomM is the authority: a game's saves go up in the form they're there in.
 *   zip   the game's .gci files zipped (this app's own form, and the default)
 *   gci   a single .gci, as other apps send it
 *   card  a whole memory card (.raw), as Dolphin keeps one: this console's
 *         saves for the game are put on a copy of RomM's card (kept in
 *         gccards/<rom id>.raw), so the card's other saves stay as they are. */

static const char *gc_form_of_name(const char *file_name) {
    if (str_ends_with_ci(file_name, ".gci")) return "gci";
    if (ext_in_list(file_name, ".raw,.gcp,.mcp")) return "card";
    return "zip";
}

static void gc_card_cache(int rom_id, char *out, size_t n) {
    char dir[PATH_MAX_LEN];
    path_join(dir, sizeof dir, plat_data_dir(), "gccards");
    snprintf(out, n, "%s/%d.raw", dir, rom_id);
}

static void gc_form_get(const local_rom *r, char *form, size_t n, char *name, size_t nn) {
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    str_copy(form, n, e ? jget_str(e, "gc_form", "zip") : "zip");
    if (name) str_copy(name, nn, e ? jget_str(e, "gc_name", "") : "");
    state_unlock();
}

static void gc_form_set(const local_rom *r, const char *form, const char *name) {
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    if (e) {
        jset_str(e, "gc_form", form);
        if (name && name[0]) jset_str(e, "gc_name", name);
    }
    state_unlock();
}

/* Fetches a save's content without telling RomM this console has it. */
static int gc_fetch(int save_id, const char *dest) {
    char api[64];
    snprintf(api, sizeof api, "/api/saves/%d/content", save_id);
    http_resp resp;
    mkdir_parent(dest);
    if (romm_download(api, dest, NULL, NULL, &resp) != 0) {
        LOGW("save %d: %s", save_id, resp.error);
        unlink(dest);
        return -1;
    }
    return 0;
}

static int gc_is_card_file(const char *path) {
    size_t len = 0;
    char *data = read_file(path, &len);
    gc_card card;
    int ok = data && gc_card_open(&card, (const unsigned char *)data, len) == 0;
    if (ok) gc_card_free(&card);
    free(data);
    return ok;
}

/* Which form RomM has the game's saves in: its newest one in this console's
 * slot, or else its newest one anywhere. For a card, a copy of it is kept
 * here so this console's saves can be put on it. */
static void gc_learn_form(const scan_ctx *c, const local_rom *r) {
    char api[64];
    snprintf(api, sizeof api, "/api/saves?rom_id=%d", r->rom_id);
    long st = 0;
    cJSON *all = romm_call("GET", api, NULL, &st);
    const cJSON *s, *mine = NULL, *any = NULL;
    cJSON_ArrayForEach(s, (st == 200 ? all : NULL)) {
        if ((int)jget_num(s, "rom_id", 0) != r->rom_id) continue;
        const char *up = jget_str(s, "updated_at", "");
        if (!any || strcmp(up, jget_str(any, "updated_at", "")) > 0) any = s;
        if (!strcmp(jget_str(s, "slot", ""), c->slot) && (!mine || strcmp(up, jget_str(mine, "updated_at", "")) > 0))
            mine = s;
    }
    const cJSON *pick = mine ? mine : any;
    if (!pick && st == 200) { /* nothing on RomM: this app's own form, no card kept */
        char cache[PATH_MAX_LEN];
        gc_card_cache(r->rom_id, cache, sizeof cache);
        unlink(cache);
        gc_form_set(r, "zip", NULL);
    }
    if (pick) {
        /* RomM stamps the names it keeps: "MemoryCardA.USA [2026-10-14_16-39-01].raw". */
        char name[256];
        str_copy(name, sizeof name, jget_str(pick, "file_name", ""));
        char *open = strrchr(name, '['), *close = open ? strchr(open, ']') : NULL;
        if (open && close && open > name && open[-1] == ' ') memmove(open - 1, close + 1, strlen(close + 1) + 1);
        const char *form = gc_form_of_name(name);
        char was[8];
        gc_form_get(r, was, sizeof was, NULL, 0);
        if (strcmp(was, form) != 0) LOGI("%s: RomM keeps its saves as %s (%s)", path_basename(r->path), form, name);
        gc_form_set(r, form, name);
        char cache[PATH_MAX_LEN];
        gc_card_cache(r->rom_id, cache, sizeof cache);
        if (strcmp(form, "card") != 0) unlink(cache); /* RomM keeps no card for it now */
        if (!strcmp(form, "card") && !file_exists(cache) && gc_fetch((int)jget_num(pick, "id", 0), cache) == 0 &&
            !gc_is_card_file(cache)) {
            LOGW("%s: RomM's save %s isn't a GameCube memory card", path_basename(r->path), name);
            unlink(cache);
        }
    }
    cJSON_Delete(all);
}

/* .gci files taken off a card. */
typedef struct {
    unsigned char *g[32];
    size_t n[32];
    int count;
} gc_list;

static void gc_keep(const unsigned char *g, size_t n, void *ud) {
    gc_list *l = ud;
    if (l->count >= 32 || !(l->g[l->count] = malloc(n))) return;
    memcpy(l->g[l->count], g, n);
    l->n[l->count++] = n;
}

static void gc_list_free(gc_list *l) {
    for (int k = 0; k < l->count; k++) free(l->g[k]);
    l->count = 0;
}

/* RomM's card with this console's saves for the game on it, in place of the
 * card's own. Unchanged saves leave the card exactly as RomM has it. */
static int gc_virtual_card(int rom_id, const bundle *b, unsigned char **out, size_t *out_n) {
    char cache[PATH_MAX_LEN];
    gc_card_cache(rom_id, cache, sizeof cache);
    size_t len = 0;
    char *raw = read_file(cache, &len);
    gc_card card;
    if (!raw || gc_card_open(&card, (const unsigned char *)raw, len) != 0) {
        free(raw);
        return -1;
    }
    free(raw);
    /* Same saves, same bytes (where each sits on the card aside)? */
    gc_list on_card = {0};
    gc_card_saves(&card, b->game_id, gc_keep, &on_card);
    int same = on_card.count == b->n;
    for (int i = 0; same && i < b->n; i++) {
        size_t gl = 0;
        char *g = read_file(b->files[i].path, &gl);
        int match = 0;
        for (int k = 0; g && k < on_card.count && !match; k++)
            match = on_card.n[k] == gl && !memcmp(on_card.g[k], g, 0x36) &&
                    !memcmp(on_card.g[k] + 0x38, g + 0x38, gl - 0x38);
        same = match;
        free(g);
    }
    gc_list_free(&on_card);
    int rc = 0;
    if (!same) {
        gc_card_remove(&card, b->game_id);
        for (int i = 0; i < b->n && rc == 0; i++) {
            size_t gl = 0;
            char *g = read_file(b->files[i].path, &gl);
            if (!g || gc_card_put(&card, (const unsigned char *)g, gl) != 0) {
                LOGW("%s doesn't fit on RomM's memory card", b->files[i].name);
                rc = -1;
            }
            free(g);
        }
    }
    if (rc == 0) {
        *out = card.data;
        *out_n = card.size;
        card.data = NULL;
    }
    gc_card_free(&card);
    return rc;
}

/* The one .gci that goes up when RomM keeps a single one: the same save as
 * RomM's, or the newest. */
static int gc_pick_gci(const bundle *b, const char *server_name, char *out, size_t n) {
    time_t best = 0;
    out[0] = 0;
    for (int i = 0; i < b->n; i++)
        if (server_name && !strcmp(b->files[i].name, server_name)) {
            str_copy(out, n, b->files[i].path);
            return 0;
        }
    for (int i = 0; i < b->n; i++) {
        time_t t = file_mtime(b->files[i].path);
        if (!out[0] || t > best) {
            best = t;
            str_copy(out, n, b->files[i].path);
        }
    }
    return out[0] ? 0 : -1;
}

/* The unit sigil makes of a PS2 game's saves; 0 with *unit NULL when it has none. */
static int ps2_unit(const local_rom *r, const char *serial, unsigned char **unit, size_t *len, char hash[33],
                    char name[256], char identity[33]) {
    ps2_cards pc;
    ps2_cards_for(r, &pc);
    return ps2card_collect(&pc, r->path, serial, unit, len, hash, name, identity);
}

/* Its hash, name and identity, kept per game while the files it's made of stay
 * as they are: making it reads the whole card. */
static void ps2_unit_hash(const local_rom *r, const bundle *b, char hash[33], char name[256], char identity[33]) {
    ps2_cards pc;
    ps2_cards_for(r, &pc);
    char key[640], card[PATH_MAX_LEN + 32];
    snprintf(key, sizeof key, "%lld:%lld:%d:%s:%s", (long long)b->mtime, (long long)b->size, b->n, pc.slot[0], pc.slot[1]);
    /* Kept per card, not per game file: emulators playing one file through
     * a mount each have their own card, and would undo each other's. */
    snprintf(card, sizeof card, "%s#%s", b->dir, b->game_id);
    state_lock();
    cJSON *e = state_entry("ps2_units", card);
    /* An entry from before identities were kept is made again. */
    int fresh = e && !strcmp(jget_str(e, "ps2_key", ""), key) && jget_str(e, "ps2_identity", "")[0];
    if (fresh) {
        str_copy(hash, 33, jget_str(e, "ps2_hash", ""));
        str_copy(name, 256, jget_str(e, "ps2_name", ""));
        str_copy(identity, 33, jget_str(e, "ps2_identity", ""));
    }
    state_unlock();
    if (fresh) return;
    unsigned char *unit = NULL;
    size_t len = 0;
    if (ps2card_collect(&pc, r->path, b->game_id, &unit, &len, hash, name, identity) != 0) hash[0] = identity[0] = 0;
    free(unit);
    state_lock();
    e = hash[0] ? state_entry_ensure("ps2_units", card) : NULL;
    if (e) {
        jset_str(e, "ps2_key", key);
        jset_str(e, "ps2_hash", hash);
        jset_str(e, "ps2_name", name);
        jset_str(e, "ps2_identity", identity);
    }
    state_unlock();
}

/* The saves RomM's copy holds, as of the last sync: their identity, and the
 * hash RomM has for its copy. The same saves with other timestamps (another
 * console's _pcsx2_index) make another unit, so they're told apart by
 * identity, and RomM is given its own hash for them. */
static void ps2_note_synced(const local_rom *r, const char *identity, const char *server_hash) {
    if (!identity[0] || !server_hash || !server_hash[0]) return;
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    if (e) {
        jset_str(e, "ps2_synced_identity", identity);
        jset_str(e, "ps2_synced_hash", server_hash);
    }
    state_unlock();
}

static void ps2_hash_for_romm(const local_rom *r, const char *identity, char hash[33]) {
    state_lock();
    cJSON *e = state_entry("roms", r->path);
    if (e && identity[0] && !strcmp(jget_str(e, "ps2_synced_identity", ""), identity))
        str_copy(hash, 33, jget_str(e, "ps2_synced_hash", hash));
    state_unlock();
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
    if (!strcmp(r->map->save_layout, "ps2")) {
        char identity[33];
        ps2_unit_hash(r, b, f->hash, f->upload_name, identity); /* no hash: not sent (scan_bundles) */
        str_copy(f->ps2_identity, sizeof f->ps2_identity, identity);
        ps2_hash_for_romm(r, identity, f->hash);
    } else if (!strcmp(r->map->save_layout, "psp")) {
        snprintf(f->upload_name, sizeof f->upload_name, "%s.zip", b->game_id);
    } else if (!strcmp(r->map->save_layout, "switch") || !strcmp(r->map->save_layout, "xbox") ||
               !strcmp(r->map->save_layout, "x360")) {
        snprintf(f->upload_name, sizeof f->upload_name, "%s.zip", b->game_id);
    } else if (!strcmp(r->map->save_layout, "3ds") || !strcmp(r->map->save_layout, "wiiu")) {
        /* As RomM's save units name them: the title id's low half. */
        snprintf(f->upload_name, sizeof f->upload_name, "%s.zip", strlen(b->game_id) == 16 ? b->game_id + 8 : b->game_id);
        for (char *p = f->upload_name; *p; p++) *p = (char)tolower((unsigned char)*p);
    } else {
        char stem[200];
        path_stem(stem, sizeof stem, r->path);
        snprintf(f->upload_name, sizeof f->upload_name, "%s.zip", stem);
    }
    if (strcmp(r->map->save_layout, "gci") != 0) return;
    /* In the form RomM has it: the hash RomM would give what we'd send. */
    char form[8], name[256];
    gc_form_get(r, form, sizeof form, name, sizeof name);
    if (!strcmp(form, "gci") && gc_pick_gci(b, name, f->gc_file, sizeof f->gc_file) == 0) {
        str_copy(f->gc_form, sizeof f->gc_form, "gci");
        md5_file_hex(f->gc_file, f->hash);
        str_copy(f->upload_name, sizeof f->upload_name, name[0] ? name : path_basename(f->gc_file));
    } else if (!strcmp(form, "card")) {
        unsigned char *card = NULL;
        size_t n = 0;
        str_copy(f->gc_form, sizeof f->gc_form, "card");
        if (gc_virtual_card(r->rom_id, b, &card, &n) == 0) {
            md5_hex(card, n, f->hash);
            free(card);
        } else {
            f->hash[0] = 0; /* no card to put them on: not sent (scan_bundles) */
        }
        str_copy(f->upload_name, sizeof f->upload_name, name[0] ? name : "MemoryCardA.raw");
    }
}

static void scan_bundles(scan_ctx *c) {
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (!r->rules.sync_saves || !bundle_supported(r->map->save_layout)) continue;
        char id[24];
        bundle b;
        if (rom_game_id(r, id, sizeof id) != 0) continue;
        ps2_own_card(r, id);
        if (bundle_for_rom(r, id, &b) != 0) continue;
        /* An Xbox save's folder is our own copy: the sync once xemu closes sees its changes. */
        if (strcmp(r->map->save_layout, "xbox") != 0) {
            state_lock();
            cJSON_AddItemToArray(g_bundle_roots_next, cJSON_CreateString(b.dir));
            state_unlock();
        }
        if (!strcmp(r->map->save_layout, "gci")) gc_learn_form(c, r);
        if (b.n > 0) {
            local_file f;
            fill_bundle_file(&f, c, r, &b);
            /* Another emulator's copy of the game already here: the one saved
             * last goes up (a leftover RomM never saw doesn't beat one it
             * knows), and the other gets it after (share_bundle_saves). */
            int at = -1;
            for (int k = 0; k < c->nsaves && at < 0; k++)
                if (c->saves[k].rom_id == r->rom_id && !strcmp(c->saves[k].slot, c->slot)) at = k;
            if (!f.hash[0]) {
                if (strcmp(r->map->save_layout, "ps2") != 0) /* ps2card logged why */
                    LOGW("%s: its saves can't go on RomM's memory card yet", path_basename(r->path));
            } else if (at < 0) {
                add_file(&c->saves, &c->nsaves, &c->capsaves, &f);
            } else if (c->saves[at].is_bundle && !strcmp(c->saves[at].map->save_layout, r->map->save_layout) &&
                       beats(counts(f.path, f.map, f.mtime), f.mtime,
                             counts(c->saves[at].path, c->saves[at].map, c->saves[at].mtime), c->saves[at].mtime)) {
                c->saves[at] = f;
            }
        }
        bundle_free(&b);
    }
}

/* RomM's upload limit, as its 413 gives it ("Request body exceeds the
 * maximum allowed size of 536870912 bytes"): a save zip over it isn't sent.
 * Kept a day, as the server's admin can raise it (MAX_ASSET_UPLOAD_SIZE_BYTES
 * in RomM's environment) and nothing says when they have. 0 when unknown,
 * which a reverse proxy's 413 leaves it. */
#define UPLOAD_LIMIT_KEEP_SEC (24 * 3600)

static int64_t upload_limit(void) {
    state_lock();
    const cJSON *s = state_section("server");
    const double limit = jget_num(s, "upload_limit", 0), at = jget_num(s, "upload_limit_at", 0);
    state_unlock();
    return limit > 0 && (double)time(NULL) - at < UPLOAD_LIMIT_KEEP_SEC ? (int64_t)limit : 0;
}

static void learn_upload_limit(const cJSON *reply) {
    static const char said[] = "maximum allowed size of ";
    const char *at = strstr(jget_str(reply, "detail", ""), said);
    const long long limit = at ? strtoll(at + sizeof said - 1, NULL, 10) : 0;
    if (limit <= 0) return;
    state_lock();
    cJSON *s = state_section("server");
    jset_num(s, "upload_limit", (double)limit);
    jset_num(s, "upload_limit_at", (double)time(NULL));
    state_unlock();
}

/* A GameCube save from RomM that's a memory card or a single .gci: the
 * game's saves on it replace the game's .gci files here. 1 when placed, 0
 * when it's neither (a zip), -1 when it can't be used. The bundle is listed
 * again. */
static int gc_place(const local_rom *r, bundle *b, const char *file) {
    size_t len = 0;
    char *data = read_file(file, &len);
    if (!data) return -1;
    const unsigned char *u = (const unsigned char *)data;
    gc_card card;
    int rc = 0;
    if (gc_card_open(&card, u, len) == 0) {
        gc_list on_card = {0};
        gc_card_saves(&card, b->game_id, gc_keep, &on_card);
        if (on_card.count) {
            for (int i = 0; i < b->n; i++) unlink(b->files[i].path);
            mkdir_p(b->dir);
        } else {
            LOGI("RomM's memory card has no saves for %s: keeping this console's", b->game_id);
        }
        for (int k = 0; k < on_card.count; k++) {
            char name[256], path[PATH_MAX_LEN];
            gc_gci_name(on_card.g[k], name, sizeof name);
            path_join(path, sizeof path, b->dir, name);
            if (write_file_atomic(path, on_card.g[k], on_card.n[k]) != 0) rc = -1;
        }
        gc_list_free(&on_card);
        gc_card_free(&card);
        char cache[PATH_MAX_LEN];
        gc_card_cache(r->rom_id, cache, sizeof cache);
        mkdir_parent(cache);
        if (rc == 0) rc = copy_file(file, cache) == 0 ? 1 : -1;
        if (rc > 0) gc_form_set(r, "card", NULL);
    } else if (gc_gci_valid(u, len)) {
        /* Over the same save here, or beside the others. */
        char path[PATH_MAX_LEN] = "";
        for (int i = 0; i < b->n && !path[0]; i++) {
            size_t gl = 0;
            char *g = read_file(b->files[i].path, &gl);
            if (g && gl >= GC_DENTRY && gc_gci_same_save((const unsigned char *)g, u)) str_copy(path, sizeof path, b->files[i].path);
            free(g);
        }
        if (!path[0]) {
            char name[256];
            gc_gci_name(u, name, sizeof name);
            mkdir_p(b->dir);
            path_join(path, sizeof path, b->dir, name);
        }
        rc = write_file_atomic(path, u, len) == 0 ? 1 : -1;
        if (rc > 0) gc_form_set(r, "gci", NULL);
    }
    free(data);
    if (rc > 0) {
        char id[24];
        str_copy(id, sizeof id, b->game_id);
        bundle_free(b);
        if (bundle_for_rom(r, id, b) != 0) rc = -1;
    }
    return rc;
}

static int notify_on(void);

/* The game's name for a toast: RomM's, else its file's. */
static void game_name(const local_rom *r, char *out, size_t n) {
    state_lock();
    const cJSON *e = state_entry("roms", r->path);
    str_copy(out, n, e ? jget_str(e, "name", "") : "");
    state_unlock();
    if (!out[0]) path_stem(out, n, r->path);
}

/* ---- game files a save can't be placed for ----
 * A game whose id can't be read from its file (a copy cut short on the RomM
 * server, a damaged image) can't have its save put where the emulator looks
 * for it. Syncing again won't mend that, a whole copy will: it's said once
 * for the file as it is, then the game's save is skipped quietly. */

static void size_text(char *out, size_t n, uint64_t bytes) {
    if (bytes >= 1000000000ull) snprintf(out, n, "%.1f GB", (double)bytes / 1e9);
    else snprintf(out, n, "%d MB", (int)(bytes / 1000000ull));
}

/* Whether the file's trouble was said already, as it is now; it is from here on. */
static int said_unreadable(const char *path) {
    char now[48];
    snprintf(now, sizeof now, "%lld:%lld", (long long)file_size(path), (long long)file_mtime(path));
    state_lock();
    cJSON *e = state_entry("roms", path);
    const int said = e && !strcmp(jget_str(e, "unreadable", ""), now);
    if (e && !said) {
        jset_str(e, "unreadable", now);
        state_save();
    }
    state_unlock();
    return said;
}

int game_file_problem(const char *path, const char *layout, char *out, size_t n) {
    uint64_t need = 0, have = 0;
    out[0] = 0;
    if (strcmp(layout, "xbox") != 0 || dir_exists(path) || gameid_xbox_short(path, &need, &have) != 1) return 0;
    char got[24], all[24];
    size_text(got, sizeof got, have);
    size_text(all, sizeof all, need);
    snprintf(out, n, "the game file is incomplete (%s of %s): replace it on your RomM server, then download it again", got,
             all);
    said_unreadable(path);
    return 1;
}

static void say_unreadable(const local_rom *r) {
    if (said_unreadable(r->path)) return;
    char why[256], name[256];
    if (!game_file_problem(r->path, r->map->save_layout, why, sizeof why))
        str_copy(why, sizeof why, "its game id can't be read from its file");
    LOGE("%s: its save can't sync, %s", path_basename(r->path), why);
    events_add("save", "failed", r->rom_id, r->path, emu_name(r->map), "Game file unreadable");
    if (!notify_on()) return;
    game_name(r, name, sizeof name);
    plat_notify("RommPS: %s\nIts saves can't sync: %s", name, why);
}

/* A newer save that can't go onto an emulator while it's open: said once a
 * game and emulator (again after 15 minutes), so nobody plays on the old one
 * without knowing. */
static void say_save_waits(const local_rom *r) {
    static struct { int rom_id; char profile[64]; time_t at; } told[32];
    static int next;
    LOGI("%s: its save comes down once %s is closed", path_basename(r->path), r->map->profile_name);
    const time_t now = time(NULL);
    for (int i = 0; i < 32; i++)
        if (told[i].rom_id == r->rom_id && !strcmp(told[i].profile, r->map->profile_id) && now - told[i].at < 15 * 60) return;
    const int at = next++ % 32;
    told[at].rom_id = r->rom_id;
    str_copy(told[at].profile, sizeof told[at].profile, r->map->profile_id);
    told[at].at = now;
    if (!notify_on()) return;
    char name[256];
    game_name(r, name, sizeof name);
    plat_notify("RommPS: %s\nIts newer save comes down once %s is closed", name, r->map->profile_name);
}

/* An emulator that resumes a game where it was left (quick resume) puts the
 * game's memory back as it was, with the save the game read back then: a save
 * that came in since only shows after a fresh boot, and the game's next save
 * writes the old progress over it. So whenever the game's save is replaced,
 * its resume states there go to the backups. */
static void drop_resume(const scan_ctx *c, const local_rom *r) {
    if (!r || !r->map->resume_states[0]) return;
    char dir[PATH_MAX_LEN], list[256], *save = NULL;
    if (!expected_dir(r, 1, dir, sizeof dir)) str_copy(dir, sizeof dir, r->map->state_dir);
    const char *vars[17];
    name_vars(r, "", vars);
    str_copy(list, sizeof list, r->map->resume_states);
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char name[256], path[PATH_MAX_LEN];
        str_template(name, sizeof name, t, vars);
        if (!name[0] || name[0] == '.' || name[0] == '_' || strchr(name, '{')) continue;
        path_join(path, sizeof path, dir, name);
        if (!file_exists(path)) continue;
        backup_file(c, path, r->rom_id);
        if (unlink(path) == 0) {
            LOGI("%s: set aside %s's resume state %s, so the new save loads", path_basename(r->path),
                 r->map->profile_name, name);
        }
    }
}

/* The emulators a save went to in this sync, for its toast ("now in
 * RetroArch and Crux2"); bundles counts those also counted as downloads. */
static char g_shared_to[200];
static int g_shared, g_shared_bundles;

static void note_shared(const profile_map *to, int bundle) {
    g_shared++;
    g_shared_bundles += bundle;
    const char *name = to->profile_name;
    size_t l = strlen(name);
    if (!l) return;
    for (const char *p = g_shared_to; (p = strstr(p, name)); p += l)
        if ((p == g_shared_to || p[-1] == '|') && (p[l] == '|' || !p[l])) return;
    const size_t o = strlen(g_shared_to);
    snprintf(g_shared_to + o, sizeof g_shared_to - o, "%s%s", o ? "|" : "", name);
}

/* A PS2 save from RomM (sigil's card, or a zip of save folders) onto the slot
 * cards in place of the game's saves there. 1 when placed, -1 when not. The
 * bundle is listed again. */
static int ps2_place(const local_rom *r, bundle *b, const char *file) {
    if (!emulator_closed(r->map)) {
        say_save_waits(r);
        return -1;
    }
    size_t len = 0;
    char *data = read_file(file, &len);
    ps2_cards pc;
    ps2_cards_for(r, &pc);
    /* The card in slot 1 not there yet: made blank first, as the emulator would,
     * or sigil puts the save on the card in slot 2. */
    char slot1[PATH_MAX_LEN];
    if (pc.dir[0] && pc.slot[0][0] && !strchr(pc.slot[0], '/')) {
        path_join(slot1, sizeof slot1, pc.dir, pc.slot[0]);
        if (!file_exists(slot1) && !dir_exists(slot1) && ps2card_blank(slot1) == 0)
            LOGI("%s: made its memory card, %s", path_basename(r->path), pc.slot[0]);
    }
    int rc = data && pc.dir[0] && ps2card_restore(&pc, r->path, b->game_id, (const unsigned char *)data, len) == 0 ? 1 : -1;
    free(data);
    if (rc > 0) {
        char id[24];
        str_copy(id, sizeof id, b->game_id);
        bundle_free(b);
        if (bundle_for_rom(r, id, b) != 0) rc = -1;
    }
    return rc;
}

/* Downloads a save zip and unpacks it over the game's current files. With
 * record set, the result counts as synced; a manual restore leaves it to upload.
 * -2: it came down, but none of it is the game's here (state "unplaced");
 * -3: that version did so before, and isn't tried again. A newer one is. */
static int do_download_bundle(const scan_ctx *c, int save_id, const local_rom *r, const char *slot, int session_id,
                              const char *server_hash, int record) {
    char id[24], api[256], zip[PATH_MAX_LEN], tmpdir[PATH_MAX_LEN];
    bundle b;
    if (!strcmp(r->map->save_layout, "xbox") && !emulator_closed(r->map)) {
        /* xemu would go on writing to the disk it has open, and lose this one. */
        say_save_waits(r);
        return -1;
    }
    if (rom_game_id(r, id, sizeof id) != 0 || bundle_for_rom(r, id, &b) != 0) {
        LOGE("no game id for %s, cannot place its save", path_basename(r->path));
        return -1;
    }
    char unplaced[PATH_MAX_LEN + 32], version[64];
    snprintf(unplaced, sizeof unplaced, "%s#%s", b.dir, id);
    snprintf(version, sizeof version, "%d|%s", save_id, server_hash ? server_hash : "");
    if (record) {
        state_lock();
        const int tried = !strcmp(jget_str(state_entry("unplaced", unplaced), "save", ""), version);
        state_unlock();
        if (tried) {
            bundle_free(&b);
            return -3;
        }
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
    char other[64];
    if (bundle_other_ids(&b, zip, tmpdir, other, sizeof other) == 0 && other[0]) {
        /* Folders under another title id: the game's too, from now on. Listed
         * again first, so what's here under that id is backed up below. */
        save_ids_learn(id, other);
        bundle_free(&b);
        if (bundle_for_rom(r, id, &b) != 0) goto out;
    }
    if (b.n > 0) { /* keep a copy of what's there now */
        char cur[PATH_MAX_LEN];
        snprintf(cur, sizeof cur, "%s/tmp/%s.zip", plat_data_dir(), id);
        if (bundle_zip(&b, cur) == 0) backup_file(c, cur, r->rom_id);
        unlink(cur);
    }
    int placed = !strcmp(r->map->save_layout, "gci") ? gc_place(r, &b, zip)
               : !strcmp(r->map->save_layout, "ps2") ? ps2_place(r, &b, zip)
                                                     : 0;
    if (placed < 0) goto out;
    const int replaced = placed ? 0 : bundle_replace(&b, zip, tmpdir);
    if (replaced == -1) {
        LOGE("save %d is not a usable zip", save_id);
        goto out;
    }
    if (b.n == 0 || replaced == -2) {
        /* Unpacked, and still none of it counts as the game's: synced as is,
         * it'd come down again as "missing" on every sync. Forever. */
        LOGE("save %d came down, but none of its files are %s's here (%s)", save_id, id, b.dir);
        if (record) {
            state_lock();
            jset_str(state_entry_ensure("unplaced", unplaced), "save", version);
            state_unlock();
        }
        rc = -2;
        goto out;
    }
    rc = 0;
    LOGI("downloaded save %d into %s", save_id, b.dir);
    drop_resume(c, r);
    if (record) {
        local_file f;
        fill_bundle_file(&f, c, r, &b);
        str_copy(f.slot, sizeof f.slot, slot);
        if (!strcmp(r->map->save_layout, "ps2") && server_hash && server_hash[0]) {
            /* These saves are RomM's copy now, whatever timestamps they carry here. */
            ps2_note_synced(r, f.ps2_identity, server_hash);
            str_copy(f.hash, sizeof f.hash, server_hash);
        }
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
    char card_up[PATH_MAX_LEN] = "", ps2_sent[33] = ""; /* ps2: the identity of the unit sent */
    if (!strcmp(f->gc_form, "gci")) {
        j = romm_upload("POST", path, "saveFile", f->gc_file, f->upload_name, &st);
    } else if (!strcmp(f->gc_form, "card")) {
        /* RomM's card with this console's saves for the game on it. */
        bundle b;
        unsigned char *card = NULL;
        size_t n = 0;
        snprintf(card_up, sizeof card_up, "%s/tmp/upload-%d.raw", plat_data_dir(), f->rom_id);
        if (bundle_reopen(f, &b) != 0 || gc_virtual_card(f->rom_id, &b, &card, &n) != 0 ||
            write_file_atomic(card_up, card, n) != 0) {
            bundle_free(&b);
            free(card);
            if (status_out) *status_out = 0;
            return -1;
        }
        bundle_free(&b);
        free(card);
        j = romm_upload("POST", path, "saveFile", card_up, f->upload_name, &st);
    } else if (f->is_bundle && !strcmp(f->map->save_layout, "ps2")) {
        /* sigil's card of the game's saves, as other clients send it. */
        char up[PATH_MAX_LEN], hash[33], name[256];
        unsigned char *unit = NULL;
        size_t n = 0;
        snprintf(up, sizeof up, "%s/tmp/upload-%d.ps2", plat_data_dir(), f->rom_id);
        if (ps2_unit(f->rom, f->game_id, &unit, &n, hash, name, ps2_sent) != 0 || !unit || mkdir_parent(up) != 0 ||
            write_file_atomic(up, unit, n) != 0) {
            free(unit);
            if (status_out) *status_out = 0;
            return -1;
        }
        free(unit);
        j = romm_upload("POST", path, "saveFile", up, f->upload_name, &st);
        unlink(up);
    } else if (f->is_bundle) {
        char zip[PATH_MAX_LEN];
        bundle b;
        snprintf(zip, sizeof zip, "%s/tmp/upload-%d.zip", plat_data_dir(), f->rom_id);
        int zipped = -1;
        if (bundle_reopen(f, &b) != 0 || (zipped = bundle_zip(&b, zip)) != 0) {
            bundle_free(&b);
            /* Too big for a zip is too big for RomM: the same as its 413. */
            if (status_out) *status_out = zipped == -2 ? 413 : 0;
            return -1;
        }
        bundle_free(&b);
        const int64_t limit = upload_limit(), size = file_size(zip);
        if (limit > 0 && size > limit) { /* RomM's 413, without sending it all first */
            LOGI("%s: %lld bytes zipped, over RomM's limit of %lld", f->path, (long long)size, (long long)limit);
            unlink(zip);
            if (status_out) *status_out = 413;
            return -1;
        }
        j = romm_upload("POST", path, "saveFile", zip, f->upload_name, &st);
        unlink(zip);
    } else if (!strcmp(f->map->save_format, "nds-dsv")) {
        /* DeSmuME's save without its footer, as everyone else keeps it. */
        char up[PATH_MAX_LEN], name[256];
        snprintf(up, sizeof up, "%s/tmp/upload-%d.sav", plat_data_dir(), f->rom_id);
        str_copy(name, sizeof name, path_basename(f->path));
        char *dot = strrchr(name, '.');
        if (dot) str_copy(dot, sizeof name - (size_t)(dot - name), ".sav");
        if (copy_file(f->path, up) != 0 || fit_save("nds-raw", up, NULL) != 0) {
            unlink(up);
            if (status_out) *status_out = 0;
            return -1;
        }
        j = romm_upload("POST", path, "saveFile", up, name, &st);
        unlink(up);
    } else {
        j = romm_upload("POST", path, "saveFile", f->path, path_basename(f->path), &st);
    }
    if (status_out) *status_out = st;
    if (st == 404 && f->rom) forget_gone_rom(f->rom);
    if (st == 413) learn_upload_limit(j);
    int ok = (st == 200 || st == 201) && j;
    if (card_up[0]) { /* the card RomM has now is the one kept here */
        char cache[PATH_MAX_LEN];
        gc_card_cache(f->rom_id, cache, sizeof cache);
        if (ok) move_file(card_up, cache);
        else unlink(card_up);
    }
    if (ok) {
        record_save(f, (int)jget_num(j, "id", 0), jget_str(j, "content_hash", f->hash));
        if (ps2_sent[0]) ps2_note_synced(f->rom, ps2_sent, jget_str(j, "content_hash", f->hash));
        LOGI("uploaded %s (rom %d, slot %s)", f->path, f->rom_id, f->slot);
    }
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

/* RomM turns away a save over its upload limit (HTTP 413), and would again
 * on every sync after. So the version refused is written down, and the save
 * goes up again only once it changes (and fits, when RomM said its limit).
 * The notification is for the first one; a game that saves every few
 * minutes would make a hobby of it otherwise. */
static int refused_as_too_big(const local_file *f) {
    state_lock();
    const cJSON *e = state_entry("too_big", f->path);
    const int same = e && f->hash[0] && !strcmp(jget_str(e, "hash", ""), f->hash);
    state_unlock();
    return same;
}

static void say_too_big(const local_file *f) {
    state_lock();
    const int first = state_entry("too_big", f->path) == NULL;
    jset_str(state_entry_ensure("too_big", f->path), "hash", f->hash);
    state_save();
    state_unlock();
    const long long limit = (long long)upload_limit();
    if (limit) LOGW("%s: over RomM's upload limit of %lld bytes; sent again once it changes", f->path, limit);
    else LOGW("%s: too big for the server; sent again once it changes", f->path);
    events_add("save", "failed", f->rom_id, f->path, emu_name(f->map), "Too big for RomM");
    if (!first || !notify_on() || !f->rom) return;
    char name[256];
    game_name(f->rom, name, sizeof name);
    /* RomM's limit is a setting, which is worth knowing; a proxy's isn't ours to name. */
    if (limit) plat_notify("RommPS: %s\nIts save is over RomM's %lld MB upload limit (MAX_ASSET_UPLOAD_SIZE_BYTES)", name, limit >> 20);
    else plat_notify("RommPS: %s\nIts save is too big for the server", name);
}

/* Where a downloaded save goes when the game has no local save yet. */
static void active_core(const profile_map *m, char *out, size_t n);

/* The save name the emulator uses with the core it runs now. */
static void save_name_now(const profile_map *m, char *out, size_t n) {
    str_copy(out, n, m->save_name);
    if (!m->save_names_by_core[0]) return;
    char core[64], list[256], *save = NULL;
    active_core(m, core, sizeof core);
    str_copy(list, sizeof list, m->save_names_by_core);
    for (char *t = strtok_r(list, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(t, core)) str_copy(out, n, eq + 1);
    }
}

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
    const char *vars[17];
    name_vars(r, ext, vars);
    char name[512], dir[PATH_MAX_LEN], tmpl[128];
    save_name_now(r->map, tmpl, sizeof tmpl);
    if (!expected_dir(r, 0, dir, sizeof dir)) str_copy(dir, sizeof dir, r->map->save_dir);
    /* A name from a value this game lacks (no serial read) falls back to its own. */
    if (first_known_name(tmpl, vars, dir, name, sizeof name) != 0)
        str_template(name, sizeof name, "{rom_stem}{ext}", vars);
    path_join(out, n, dir, name);
}

/* Returns 0, -1 when it failed, or -2 when it would put a blank PS1 card over
 * one with saves on it (only with guard; the user choosing RomM's copy isn't). */
static int do_download(const scan_ctx *c, int save_id, const char *dest, const local_file *f,
                       const local_rom *game, int rom_id, const char *slot, int session_id, const char *server_hash,
                       const char *server_updated_at, int guard) {
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
    if (guard && f && psx_card_saves(f->map, tmp) == 0 && psx_card_saves(f->map, dest) > 0) {
        LOGW("save %d is a blank memory card and %s has saves on it: not replacing it", save_id, dest);
        unlink(tmp);
        return -2;
    }
    char sent[33] = ""; /* what RomM has, before the footer goes on or off */
    md5_file_hex(tmp, sent);
    const profile_map *into = f ? f->map : game ? game->map : NULL;
    if (into && fit_save(into->save_format, tmp, dest) != 0) {
        LOGE("save %d: can't make it %s's", save_id, into->profile_name);
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
    mark_fresh(dest);

    local_file rec;
    memset(&rec, 0, sizeof rec);
    if (f) rec = *f;
    str_copy(rec.path, sizeof rec.path, dest);
    rec.rom_id = rom_id;
    str_copy(rec.slot, sizeof rec.slot, slot);
    char hash[33] = "", now[33] = "";
    save_md5(dest, hash);
    /* Footer put on or taken off: written down as RomM's, so the next sync
     * sees the two in step (one DeSmuME sent with its footer would otherwise
     * come down again every time). Once the file changes, its own counts. */
    if (sent[0] && md5_file_hex(dest, now) == 0 && strcmp(now, sent) != 0) str_copy(hash, sizeof hash, sent);
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
    drop_resume(c, game);
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

/* The core the emulator's log says it started last (the last "log_line"
 * in it), by "log_names"; 0 when it says none of them. */
static int logged_core(const profile_map *m, char *out, size_t n) {
    if (!m->core_log[0] || !m->core_log_line[0]) return 0;
    char *txt = read_file(m->core_log, NULL), name[64] = "";
    for (char *l = txt ? strstr(txt, m->core_log_line) : NULL; l; l = strstr(l + 1, m->core_log_line)) {
        size_t k = 0;
        for (const char *p = l + strlen(m->core_log_line); *p && *p != '\n' && *p != '\r' && k + 1 < sizeof name; p++)
            name[k++] = (char)tolower((unsigned char)*p);
        name[k] = 0;
    }
    free(txt);
    char list[256], *save = NULL;
    str_copy(list, sizeof list, m->core_log_names);
    for (char *t = strtok_r(list, ",", &save); t && name[0]; t = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        for (char *c = t; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (strstr(name, t)) {
            str_copy(out, n, eq + 1);
            return 1;
        }
    }
    return 0;
}

/* The core an emulator runs now, from its own settings file
 * ("state_core_setting"); the first listed value when the file says nothing.
 * Left to the emulator ("auto", or nothing set), its log says which. */
static void active_core(const profile_map *m, char *out, size_t n) {
    out[0] = 0;
    char value[32] = "";
    char *txt = m->core_key[0] ? read_file(m->core_file, NULL) : NULL;
    for (char *l = txt; l && *l;) {
        size_t kl = strlen(m->core_key);
        if (!strncmp(l, m->core_key, kl) && l[kl] == '=') {
            size_t v = 0;
            for (l += kl + 1; *l && *l != '\n' && *l != '\r' && v + 1 < sizeof value; l++) value[v++] = *l;
            value[v] = 0;
            break;
        }
        l = strchr(l, '\n');
        if (l) l++;
    }
    free(txt);
    if (m->core_auto[0] && (!value[0] || !strcmp(value, m->core_auto)) && logged_core(m, out, n)) return;
    char list[256];
    str_copy(list, sizeof list, m->core_values);
    char *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        if (!out[0] || !strcmp(t, value)) str_copy(out, n, eq + 1);
        if (!strcmp(t, value)) break;
    }
}

/* Whether an emulator can load a state RomM says `emulator` made. */
static int map_takes_state(const profile_map *m, const char *emulator) {
    char want[64], key[64];
    emulator_key(emulator ? emulator : "", want, sizeof want);
    if (!want[0]) return 0;
    if (m->core_values[0]) {
        char core[64];
        active_core(m, core, sizeof core);
        emulator_key(core, key, sizeof key);
        if (strcmp(key, want) != 0) return 0;
    }
    char list[256];
    str_copy(list, sizeof list, m->state_cores);
    char *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        emulator_key(t, key, sizeof key);
        if (!strcmp(key, want)) return 1;
    }
    return 0;
}

/* The local copy of a game whose emulator can load a state from `emulator`. */
/* A game here whose emulator takes no save (or state): which, and why. 0 when
 * the game isn't here at all. */
static int why_no_sync(const scan_ctx *c, int rom_id, int states, char *err, int en) {
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (r->rom_id != rom_id) continue;
        if (!r->rules.sync_saves) {
            const char *why = r->rules.sync_note[0] ? r->rules.sync_note : r->map->sync_note;
            snprintf(err, (size_t)en, "%s: saves don't sync.%s%s", r->map->profile_name, why[0] ? " " : "", why);
            return 1;
        }
        if (states && !r->map->sync_states) {
            snprintf(err, (size_t)en, "%s: save states don't sync.", r->map->profile_name);
            return 1;
        }
    }
    return 0;
}

static const local_rom *find_rom_for_state(const scan_ctx *c, int rom_id, const char *emulator) {
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (r->rom_id == rom_id && r->rules.sync_saves && r->map->sync_states && map_takes_state(r->map, emulator)) return r;
    }
    return NULL;
}

/* The emulator as the log names it: "RetroArch (mGBA)", "PSXS5". */
static const char *emu_name(const profile_map *m) {
    static __thread char out[160];
    if (m->core_name[0]) snprintf(out, sizeof out, "%s (%s)", m->profile_name, m->core_name);
    else str_copy(out, sizeof out, m->profile_name);
    return out;
}

/* Saves from before RomM's sync slots (no slot), or kept in another device's
 * slot: this console's sync never brings them.
 * A game here with no save of its own gets the newest one, and from then on
 * syncs it in this console's slot. Never over a file that's there. */
static int fetch_slotless_saves(scan_ctx *c, int only_rom) {
    int want = 0;
    for (int i = 0; i < c->nroms && !want; i++) {
        const local_rom *r = &c->roms[i];
        want = r->rules.sync_saves && !r->map->save_layout[0] && (!only_rom || r->rom_id == only_rom) &&
               !find_save(c, r->rom_id, c->slot);
    }
    if (!want) return 0;
    long st = 0;
    char api[64] = "/api/saves";
    if (only_rom) snprintf(api, sizeof api, "/api/saves?rom_id=%d", only_rom);
    cJSON *all = romm_call("GET", api, NULL, &st);
    int n = 0;
    for (int i = 0; st == 200 && i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (!r->rules.sync_saves || r->map->save_layout[0] || (only_rom && r->rom_id != only_rom) ||
            find_save(c, r->rom_id, c->slot))
            continue;
        int done_before = 0; /* another emulator's copy of the game took it */
        for (int k = 0; k < i; k++) done_before |= c->roms[k].rom_id == r->rom_id;
        if (done_before) continue;
        /* The newest of each kind: the save, and files beside it such as a
         * real-time clock (.rtc), each in the slot it would sync in. */
        struct { char slot[64]; const cJSON *s; } picks[4];
        int np = 0, slotted = 0;
        const cJSON *s;
        cJSON_ArrayForEach(s, all) {
            if ((int)jget_num(s, "rom_id", 0) != r->rom_id) continue;
            if (our_slot(c, jget_str(s, "slot", ""))) { /* the sync's own */
                slotted = 1;
                continue;
            }
            const char *name = jget_str(s, "file_name", "");
            if (!ext_in_list(name, r->map->save_exts)) continue;
            char sl[64];
            slot_for(c, r->map, name, sl, sizeof sl);
            int at = -1;
            for (int k = 0; k < np; k++)
                if (!strcmp(picks[k].slot, sl)) at = k;
            if (at < 0 && np < 4) {
                at = np++;
                str_copy(picks[at].slot, sizeof picks[at].slot, sl);
                picks[at].s = s;
            } else if (at >= 0 && strcmp(jget_str(s, "updated_at", ""), jget_str(picks[at].s, "updated_at", "")) > 0) {
                picks[at].s = s;
            }
        }
        if (slotted) continue; /* the sync's own job */
        for (int k = 0; k < np; k++) {
            if (find_save(c, r->rom_id, picks[k].slot)) continue;
            char dest[PATH_MAX_LEN];
            download_target(r, jget_str(picks[k].s, "file_name", ""), picks[k].slot, c, dest, sizeof dest);
            if (file_exists(dest)) continue;
            mkdir_parent(dest);
            if (do_download(c, (int)jget_num(picks[k].s, "id", 0), dest, NULL, r, r->rom_id, picks[k].slot, 0, "",
                            jget_str(picks[k].s, "updated_at", ""), 0) == 0) {
                n++;
                events_add("save", "downloaded", r->rom_id, dest, emu_name(r->map),
                           jget_str(picks[k].s, "slot", "")[0] ? "From another device" : "Older save with no slot");
            }
        }
    }
    cJSON_Delete(all);
    return n;
}

/* Saves synced before whose file is gone from this console (deleted, or lost
 * with the emulator): RomM sees nothing new, so negotiate never offers them.
 * The newest version of that slot comes back to where it was. To get rid of a
 * save for good, delete it on RomM too. */
/* RomM's saves of a game, asked once a sync whatever needs them (restoring
 * missing saves, sharing between emulators): one request a game, not one an
 * emulator. NULL when RomM didn't answer. slotless: the caller can use a save
 * uploaded on RomM's page too. */
static const cJSON *romm_saves_of(scan_ctx *c, int rom_id, int slotless) {
    char key[16], api[64];
    snprintf(key, sizeof key, "%d", rom_id);
    if (!c->romm_saves) c->romm_saves = cJSON_CreateObject();
    const cJSON *had = cJSON_GetObjectItemCaseSensitive(c->romm_saves, key);
    if (had) return cJSON_IsArray(had) ? had : NULL;
    /* Negotiate listed every game RomM has saves of in a slot: none for this
     * one. Not the saves uploaded on RomM's page, which have no slot. */
    if (!slotless && c->romm_has && !cJSON_GetObjectItemCaseSensitive(c->romm_has, key)) {
        cJSON *none = cJSON_CreateArray();
        cJSON_AddItemToObject(c->romm_saves, key, none);
        return none;
    }
    snprintf(api, sizeof api, "/api/saves?rom_id=%d", rom_id);
    long st = 0;
    cJSON *all = romm_call("GET", api, NULL, &st);
    if (st != 200 || !cJSON_IsArray(all)) {
        cJSON_Delete(all);
        cJSON_AddNullToObject(c->romm_saves, key); /* not asked again this sync */
        return NULL;
    }
    cJSON_AddItemToObject(c->romm_saves, key, all);
    return all;
}

static int restore_missing_saves(scan_ctx *c, int only_rom) {
    typedef struct { char path[PATH_MAX_LEN]; char slot[64]; int rom_id, save_id; } missing;
    missing *list = NULL;
    int nlist = 0, cap = 0;
    state_lock();
    const cJSON *e;
    cJSON_ArrayForEach(e, state_section("saves")) {
        const char *path = e->string;
        int rom_id = (int)jget_num(e, "rom_id", 0);
        if (!rom_id || (only_rom && rom_id != only_rom) || strchr(path, '#') || file_exists(path)) continue;
        if (nlist == cap) {
            cap = cap ? cap * 2 : 16;
            missing *grown = realloc(list, (size_t)cap * sizeof *list);
            if (!grown) break;
            list = grown;
        }
        str_copy(list[nlist].path, sizeof list[nlist].path, path);
        str_copy(list[nlist].slot, sizeof list[nlist].slot, jget_str(e, "slot", ""));
        list[nlist].save_id = (int)jget_num(e, "save_id", 0);
        list[nlist++].rom_id = rom_id;
    }
    state_unlock();
    int n = 0;
    for (int i = 0; i < nlist; i++) {
        const missing *m = &list[i];
        /* The emulator whose folder the file was in (melonDS's .sav or
         * DeSmuME's .dsv: they take the save in different forms), else the
         * game's first. */
        const local_rom *r = NULL, *first = NULL;
        for (int k = 0; k < c->nroms && !r; k++) {
            const local_rom *x = &c->roms[k];
            if (x->rom_id != m->rom_id || !x->rules.sync_saves || x->map->save_layout[0]) continue;
            if (!first) first = x;
            char dir[PATH_MAX_LEN];
            if (!expected_dir(x, 0, dir, sizeof dir)) str_copy(dir, sizeof dir, x->rules.save_dir);
            const size_t dl = strlen(dir);
            if (dl && !strncmp(m->path, dir, dl) && m->path[dl] == '/') r = x;
        }
        if (!r) r = first;
        if (!r || find_save(c, m->rom_id, m->slot) || file_exists(m->path)) continue; /* the game's gone, or it has a save */
        int done_before = 0; /* one file per slot: another name of the same save came back already */
        for (int k = 0; k < i; k++) done_before |= list[k].rom_id == m->rom_id && !strcmp(list[k].slot, m->slot) && file_exists(list[k].path);
        if (done_before) continue;
        const cJSON *all = romm_saves_of(c, m->rom_id, 0);
        const cJSON *s, *best = NULL;
        cJSON_ArrayForEach(s, all)
            if ((int)jget_num(s, "rom_id", 0) == m->rom_id && !strcmp(jget_str(s, "slot", ""), m->slot) &&
                (!best || strcmp(jget_str(s, "updated_at", ""), jget_str(best, "updated_at", "")) > 0))
                best = s;
        /* Two names for one slot (a PS1 card by serial and by file name): the
         * one that had RomM's newest version gets it. */
        int other = 0;
        const int best_id = best ? (int)jget_num(best, "id", 0) : 0;
        for (int k = i + 1; best && m->save_id != best_id && k < nlist; k++)
            other |= list[k].rom_id == m->rom_id && !strcmp(list[k].slot, m->slot) && list[k].save_id == best_id;
        if (best && !other) {
            mkdir_parent(m->path);
            if (do_download(c, (int)jget_num(best, "id", 0), m->path, NULL, r, m->rom_id, m->slot, 0,
                            jget_str(best, "content_hash", ""), jget_str(best, "updated_at", ""), 0) == 0) {
                n++;
                events_add("save", "downloaded", m->rom_id, m->path, emu_name(r->map), "Missing here");
            }
        }
    }
    free(list);
    return n;
}

/* A game whose saves are a folder or a card (GameCube, PSP, Wii, PS2) with
 * none of them in an emulator while RomM has some: a new emulator, files
 * deleted, or a second emulator of the game. RomM's newest in this console's
 * slot (or else anywhere) comes down. Each emulator is looked at on its own:
 * the sync sees the game's save in the first one, the same as RomM's, and
 * brings nothing to the second. One sharing a folder with the first finds the
 * saves already there. */
static int restore_missing_bundles(scan_ctx *c, int only_rom) {
    int n = 0;
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        if (!r->rules.sync_saves || !bundle_supported(r->map->save_layout) || (only_rom && r->rom_id != only_rom)) continue;
        char id[24];
        bundle b = {0};
        if (rom_game_id(r, id, sizeof id) != 0 || bundle_for_rom(r, id, &b) != 0) continue;
        const int empty = b.n == 0;
        bundle_free(&b);
        if (!empty) continue;
        const cJSON *all = romm_saves_of(c, r->rom_id, 1);
        const cJSON *s, *mine = NULL, *any = NULL;
        cJSON_ArrayForEach(s, all) {
            if ((int)jget_num(s, "rom_id", 0) != r->rom_id) continue;
            const char *up = jget_str(s, "updated_at", "");
            if (!any || strcmp(up, jget_str(any, "updated_at", "")) > 0) any = s;
            if (!strcmp(jget_str(s, "slot", ""), c->slot) && (!mine || strcmp(up, jget_str(mine, "updated_at", "")) > 0))
                mine = s;
        }
        const cJSON *pick = mine ? mine : any;
        if (!pick) continue;
        const int rc = do_download_bundle(c, (int)jget_num(pick, "id", 0), r, c->slot, 0, jget_str(pick, "content_hash", ""), 1);
        if (rc == 0) {
            n++;
            events_add("save", "downloaded", r->rom_id, b.dir, emu_name(r->map), "Missing here");
        } else if (rc == -2) {
            events_add("save", "failed", r->rom_id, b.dir, emu_name(r->map), "RomM's save doesn't match this game here");
        }
    }
    return n;
}

/* The emulators of a game sharing its saves as cards or folders (PS2, PSP,
 * Wii U, 3DS; GameCube has share_bundles): when their copies differ, the one
 * saved last went up (scan_bundles), and the others get RomM's newest, what
 * they had backed up first. Copies are told apart by what's in them: a PS2
 * game's by its saves (identity), not the card's timestamps. */
static int same_bundle_group(const local_rom *a, const local_rom *b) {
    return a->rom_id == b->rom_id && !strcmp(a->map->save_layout, b->map->save_layout) && b->rules.sync_saves &&
           !b->map->own_slot;
}

static int share_bundle_saves(scan_ctx *c, int only_rom) {
    int n = 0;
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *a = &c->roms[i];
        if (!a->rules.sync_saves || a->map->own_slot || !bundle_supported(a->map->save_layout) ||
            !strcmp(a->map->save_layout, "gci") || (only_rom && a->rom_id != only_rom))
            continue;
        int first = 1;
        for (int k = 0; k < i && first; k++) first = !same_bundle_group(a, &c->roms[k]);
        char id[24];
        if (!first || rom_game_id(a, id, sizeof id) != 0) continue;
        struct { const local_rom *r; char print[33], dir[PATH_MAX_LEN]; time_t mtime; int synced; } copies[4];
        int nc = 0, newest = -1;
        for (int k = i; k < c->nroms && nc < 4; k++) {
            const local_rom *r = &c->roms[k];
            bundle b;
            if (!same_bundle_group(a, r) || bundle_for_rom(r, id, &b) != 0) continue;
            if (b.n > 0) { /* none at all: restore_missing_bundles */
                local_file f;
                fill_bundle_file(&f, c, r, &b);
                const char *print = !strcmp(r->map->save_layout, "ps2") ? f.ps2_identity : f.hash;
                if (print[0]) {
                    copies[nc].r = r;
                    copies[nc].mtime = b.mtime;
                    copies[nc].synced = counts(f.path, r->map, b.mtime);
                    str_copy(copies[nc].print, sizeof copies[nc].print, print);
                    str_copy(copies[nc].dir, sizeof copies[nc].dir, b.dir);
                    if (newest < 0 || beats(copies[nc].synced, b.mtime, copies[newest].synced, copies[newest].mtime))
                        newest = nc;
                    nc++;
                }
            }
            bundle_free(&b);
        }
        int differ = 0;
        for (int k = 0; k < nc; k++) differ |= strcmp(copies[k].print, copies[newest].print) != 0;
        if (nc < 2 || !differ) continue;
        const cJSON *all = romm_saves_of(c, a->rom_id, 0);
        const cJSON *s, *pick = NULL;
        cJSON_ArrayForEach(s, all)
            if ((int)jget_num(s, "rom_id", 0) == a->rom_id && !strcmp(jget_str(s, "slot", ""), c->slot) &&
                (!pick || strcmp(jget_str(s, "updated_at", ""), jget_str(pick, "updated_at", "")) > 0))
                pick = s;
        for (int k = 0; pick && k < nc; k++) {
            if (k == newest || !strcmp(copies[k].print, copies[newest].print)) continue;
            if (do_download_bundle(c, (int)jget_num(pick, "id", 0), copies[k].r, c->slot, 0,
                                   jget_str(pick, "content_hash", ""), 1) == 0) {
                n++;
                LOGI("shared save: %s's saves from %s -> %s", id, copies[newest].r->map->profile_name,
                     copies[k].r->map->profile_name);
                note_shared(copies[k].r->map, 1);
                events_add("save", "downloaded", a->rom_id, copies[k].dir, emu_name(copies[k].r->map),
                           "Saved in another emulator");
            }
        }
    }
    return n;
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
    if (is_new) {
        LOGW("conflict for %s: %s", f->path, jget_str(op, "reason", ""));
        events_add("save", "conflict", f->rom_id, f->path, emu_name(f->map), "Changed in both places");
    }
}


/* Server states already brought down or sent up, by id, with the updated_at
 * we saw. Caller holds state_lock. */
static int same_time(const char *a, const char *b) {
    time_t ta = iso8601_parse(a), tb = iso8601_parse(b);
    return ta > 0 ? ta == tb : !strcmp(a, b);
}

static void mark_seen(int id, const char *updated_at) {
    char key[16];
    snprintf(key, sizeof key, "%d", id);
    cJSON *seen = state_section("states_seen");
    cJSON_DeleteItemFromObjectCaseSensitive(seen, key);
    cJSON_AddStringToObject(seen, key, updated_at);
}

static int was_seen(int id, const char *updated_at) {
    char key[16];
    snprintf(key, sizeof key, "%d", id);
    const char *at = jget_str(state_section("states_seen"), key, NULL);
    return at && same_time(at, updated_at);
}

/* The local file holding server state id, if we have one. */
static cJSON *state_record_for(int id, char *path, size_t n) {
    cJSON *e;
    cJSON_ArrayForEach(e, state_section("states")) {
        if ((int)jget_num(e, "state_id", 0) == id) {
            str_copy(path, n, e->string);
            return e;
        }
    }
    return NULL;
}

/* Name a state goes up under: the local name with this console's name in it,
 * so other devices' states (and RomM, which replaces a state of the same
 * name) never meet it. "Game.state1" -> "Game [PlayStation 5].state1". */
static void state_upload_name(const char *path, char *out, size_t n) {
    char device[64];
    config_lock();
    str_copy(device, sizeof device, g_cfg.device_name[0] ? g_cfg.device_name : "RommPS");
    config_unlock();
    const char *base = path_basename(path), *ext = path_ext(base);
    snprintf(out, n, "%.*s [%s]%s", (int)(ext - base), base, device, ext);
}

/* A local state as RomM's web player loads it, in a temp file: RetroArch's
 * container, uncompressed. *core becomes the core a bare state comes from
 * when its signature says (PSXS5 runs several). */
static int state_for_romm(const char *path, char *tmp, size_t n, const char **core) {
    static int seq;
    snprintf(tmp, n, "%s/tmp/state-up-%d.bin", plat_data_dir(), __sync_add_and_fetch(&seq, 1));
    /* File to file, never whole in memory: states run to tens of MB (a
     * GameCube one), more than the PS4's payload can hold. */
    unsigned char head[64] = {0};
    FILE *f = fopen(path, "rb");
    size_t got = f ? fread(head, 1, sizeof head, f) : 0;
    if (f) fclose(f);
    state_kind kind = state_kind_of(head, got);
    if (kind == STATE_RAW) {
        const char *sig = state_core_of(head, got); /* a core's signature opens its state */
        if (sig && core) *core = sig;
    }
    int rc = kind == STATE_RZIP ? state_unrzip_file(path, tmp)
             : kind == STATE_RASTATE ? copy_file(path, tmp)
                                     : state_wrap_file(path, tmp);
    if (rc) LOGW("can't read the state %s", path);
    return rc ? -1 : 0;
}

static int sync_states_up(scan_ctx *c, int only_rom) {
    int uploaded = 0;
    for (int i = 0; i < c->nstates; i++) {
        local_file *f = &c->states[i];
        if (only_rom && f->rom_id != only_rom) continue;
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
        int mine = e && jget_num(e, "mine", 0);
        state_unlock();
        if (same) continue;

        char path[512];
        long st = 0;
        cJSON *j = NULL;
        const char *core = f->map->emulator; /* or the core a bare state's signature names */
        /* Only a state this console made is replaced on RomM. One that came
         * from elsewhere and was then saved over here goes up as a new one. */
        if (state_id && mine) {
            char tmp[PATH_MAX_LEN];
            if (state_for_romm(f->path, tmp, sizeof tmp, &core) == 0) {
                snprintf(path, sizeof path, "/api/states/%d", state_id);
                j = romm_upload("PUT", path, "stateFile", tmp, path_basename(f->path), &st);
                unlink(tmp);
            }
        }
        if (!state_id || !mine || st == 404) {
            cJSON_Delete(j);
            j = NULL;
            char name[300], tmp[PATH_MAX_LEN];
            state_upload_name(f->path, name, sizeof name);
            if (state_for_romm(f->path, tmp, sizeof tmp, &core) == 0) {
                char *emu = url_q(core);
                snprintf(path, sizeof path, "/api/states?rom_id=%d&emulator=%s", f->rom_id, emu);
                free(emu);
                j = romm_upload("POST", path, "stateFile", tmp, name, &st);
                unlink(tmp);
            }
        }
        if ((st == 200 || st == 201) && j) {
            int id = (int)jget_num(j, "id", state_id);
            state_lock();
            cJSON *ne = state_entry_ensure("states", f->path);
            jset_num(ne, "rom_id", f->rom_id);
            jset_str(ne, "hash", f->hash);
            jset_num(ne, "mtime", (double)f->mtime);
            char ns[24];
            snprintf(ns, sizeof ns, "%lld", (long long)file_mtime_ns(f->path));
            jset_str(ne, "mtime_ns", ns);
            jset_num(ne, "size", (double)f->size);
            jset_num(ne, "state_id", id);
            jset_num(ne, "mine", 1);
            jset_str(ne, "server_updated_at", jget_str(j, "updated_at", ""));
            jset_num(ne, "synced_at", (double)time(NULL));
            mark_seen(id, jget_str(j, "updated_at", ""));
            state_unlock();
            uploaded++;
            LOGI("uploaded state %s", f->path);
            events_add("state", "uploaded", f->rom_id, f->path, emu_name(f->map), NULL);
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
    const char *ext = path_ext(server_name), *vars[17];
    if (!strcmp(ext, ".state")) ext = r->map->state_slot0;
    name_vars(r, ext, vars);
    /* A name from a value this game lacks (no hash read): none at all. */
    if (first_known_name(r->map->state_name, vars, NULL, out, n) != 0) out[0] = 0;
}

/* A file there already, or one another server state is kept in. */
static int state_path_taken(const char *path, int id) {
    if (file_exists(path)) return 1;
    state_lock();
    const cJSON *e = state_entry("states", path);
    int taken = e && (int)jget_num(e, "state_id", 0) != id;
    state_unlock();
    return taken;
}

/* The next free numbered slot beside path ("Game.state" -> "Game.state1"),
 * for an emulator whose states are numbered. 0 if there's none. */
static int free_state_slot(const local_rom *r, char *path, size_t n, int id) {
    if (!strchr(r->map->state_exts, '*')) return 0;
    char base[PATH_MAX_LEN];
    str_copy(base, sizeof base, path);
    char *dot = strrchr(base, '.');
    if (!dot || strncmp(dot, ".state", 6) != 0) return 0;
    dot[6] = 0; /* "Game.state" */
    for (int slot = 1; slot < 100; slot++) {
        char p[PATH_MAX_LEN + 8];
        snprintf(p, sizeof p, "%s%d", base, slot);
        if (!state_path_taken(p, id)) {
            str_copy(path, n, p);
            return 1;
        }
    }
    return 0;
}

/* Downloads a state into place (backing up what was there) and records it. */
static int fetch_state(const scan_ctx *c, const cJSON *s, const char *path, const local_rom *r) {
    int rom_id = r->rom_id;
    char api[128], tmp[PATH_MAX_LEN];
    int id = (int)jget_num(s, "id", 0);
    snprintf(api, sizeof api, "/api/states/%d/content", id);
    snprintf(tmp, sizeof tmp, "%s/tmp/state-%d.bin", plat_data_dir(), id);
    http_resp resp;
    if (romm_download(api, tmp, NULL, NULL, &resp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (!strcmp(r->map->state_format, "raw")) {
        /* File to file: a state can be tens of MB, more than the PS4 holds. */
        char raw[PATH_MAX_LEN + 8];
        snprintf(raw, sizeof raw, "%s.raw", tmp);
        if (state_unwrap_file(tmp, raw) != 0 || rename(raw, tmp) != 0) {
            LOGW("state %d: can't unpack it for %s", id, path);
            unlink(raw);
            unlink(tmp);
            return -1;
        }
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
    jset_num(e, "mine", 0);
    jset_str(e, "server_updated_at", jget_str(s, "updated_at", ""));
    jset_num(e, "synced_at", (double)time(NULL));
    mark_seen(id, jget_str(s, "updated_at", ""));
    state_unlock();
    LOGI("downloaded state %s", path);
    return 0;
}

/* Logs, once per version, a state no emulator here can load: made by another
 * core, or by an app whose states only it reads. */
static void note_skipped_state(const scan_ctx *c, const cJSON *s) {
    int rom_id = (int)jget_num(s, "rom_id", 0), local = 0;
    for (int i = 0; i < c->nroms && !local; i++)
        local = c->roms[i].rom_id == rom_id && c->roms[i].rules.sync_saves && c->roms[i].map->sync_states;
    if (!local) return; /* game isn't on this console, or its emulator doesn't sync states */
    char key[16];
    snprintf(key, sizeof key, "%d", (int)jget_num(s, "id", 0));
    const char *up = jget_str(s, "updated_at", "");
    state_lock();
    cJSON *sk = state_section("states_skipped");
    int known = !strcmp(jget_str(sk, key, ""), up);
    if (!known) {
        cJSON_DeleteItemFromObjectCaseSensitive(sk, key);
        cJSON_AddStringToObject(sk, key, up);
    }
    state_unlock();
    if (!known) {
        const char *by = jget_str(s, "emulator", "")[0] ? jget_str(s, "emulator", "") : "an unknown emulator";
        LOGI("state %s skipped: made by %s, which no emulator here for rom %d can load", jget_str(s, "file_name", "?"), by,
             rom_id);
        events_add("state", "skipped", rom_id, jget_str(s, "file_name", ""), "", "No suitable emulator on console");
    }
}

/* Pulls states that are new or changed on the server. Each server state keeps
 * one local file; a new one never replaces a file that's there (the player's
 * own slot, or another server state) but takes the next free slot. A state
 * changed here since it came down is kept; the upload pass sends it. */
static int sync_states_down(scan_ctx *c, int only_rom) {
    long st = 0;
    cJSON *list = romm_call("GET", "/api/states", NULL, &st);
    int n = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, list) {
        if (only_rom && (int)jget_num(s, "rom_id", 0) != only_rom) continue;
        int id = (int)jget_num(s, "id", 0);
        const char *server_up = jget_str(s, "updated_at", "");
        char path[PATH_MAX_LEN], rec_hash[33] = "", rec_up[64] = "";
        state_lock();
        int seen = was_seen(id, server_up);
        cJSON *e = state_record_for(id, path, sizeof path);
        /* One we kept whose file is gone from here comes back. */
        const int missing = e && !file_exists(path);
        if (missing) seen = 0;
        if (e && !seen && !missing) {
            str_copy(rec_hash, sizeof rec_hash, jget_str(e, "hash", ""));
            str_copy(rec_up, sizeof rec_up, jget_str(e, "server_updated_at", ""));
            /* Recorded before states_seen existed. */
            if (same_time(rec_up, server_up)) {
                mark_seen(id, server_up);
                seen = 1;
            }
        }
        state_unlock();
        if (seen) continue;

        const char *emulator = jget_str(s, "emulator", "");
        const local_rom *r = NULL;
        if (e) { /* a state kept here: the emulator whose folder it's in (two can play the game) */
            char folder[PATH_MAX_LEN], exp[PATH_MAX_LEN];
            str_copy(folder, sizeof folder, path);
            char *slash = strrchr(folder, '/');
            if (slash) *slash = 0;
            for (int k = 0; k < c->nroms && !r; k++)
                if (c->roms[k].rom_id == (int)jget_num(s, "rom_id", 0) && c->roms[k].rules.sync_saves &&
                    c->roms[k].map->sync_states && map_takes_state(c->roms[k].map, emulator) &&
                    expected_dir(&c->roms[k], 1, exp, sizeof exp) && !strcmp(exp, folder))
                    r = &c->roms[k];
        }
        if (!r) r = find_rom_for_state(c, (int)jget_num(s, "rom_id", 0), emulator);
        if (!r) {
            note_skipped_state(c, s);
            continue;
        }
        char dir[PATH_MAX_LEN], name[256];
        if (!expected_dir(r, 1, dir, sizeof dir)) continue;
        const char *server_name = jget_str(s, "file_name", "");
        /* A folder per game renames what comes down (slot1.state): any state will do. */
        const int per_game = strstr(r->rules.state_dir, "{game_id}") != NULL;
        const char *sx = path_ext(server_name);
        if (per_game ? strncmp(sx, ".state", 6) != 0 : !ext_in_list(server_name, r->map->state_exts)) continue;

        if (e) { /* a newer version of a state we keep */
            if (file_exists(path)) {
                char hash[33];
                md5_file_hex(path, hash);
                if (strcmp(hash, rec_hash) != 0) continue;
            }
        } else if (strstr(r->rules.state_dir, "{game_id}")) {
            /* A folder per game holding slot1.state, slot2.state...: the first free. */
            int k = 1;
            for (; k < 10; k++) {
                snprintf(name, sizeof name, "slot%d.state", k);
                path_join(path, sizeof path, dir, name);
                if (!state_path_taken(path, id)) break;
            }
            if (k == 10) {
                LOGI("state %d skipped: no free slot in %s", id, dir);
                continue;
            }
        } else {
            state_local_name(r, server_name, name, sizeof name);
            if (!name[0]) continue;
            path_join(path, sizeof path, dir, name);
            if (state_path_taken(path, id) && !free_state_slot(r, path, sizeof path, id)) {
                LOGI("state %d skipped: no free slot beside %s", id, path);
                continue;
            }
        }
        if (missing) mkdir_parent(path);
        if (fetch_state(c, s, path, r) == 0) {
            n++;
            events_add("state", "downloaded", r->rom_id, path, emu_name(r->map), missing ? "Missing here" : NULL);
        } else {
            events_add("state", "failed", r->rom_id, path, emu_name(r->map), "Download failed");
        }
    }
    /* Forget states that are gone from the server, and the records of states
     * gone from both sides. */
    if (st == 200 && cJSON_IsArray(list)) {
        static const char *const sections[] = {"states_seen", "states_skipped"};
        state_lock();
        cJSON *recs = state_section("states"), *rec = recs->child;
        while (rec) {
            cJSON *next = rec->next;
            int id = (int)jget_num(rec, "state_id", 0), there = 0;
            cJSON_ArrayForEach(s, list) if ((int)jget_num(s, "id", 0) == id) there = 1;
            if (!there && !file_exists(rec->string)) cJSON_Delete(cJSON_DetachItemViaPointer(recs, rec));
            rec = next;
        }
        for (int i = 0; i < 2; i++) {
            cJSON *sec = state_section(sections[i]), *k = sec->child;
            while (k) {
                cJSON *next = k->next;
                int there = 0;
                cJSON_ArrayForEach(s, list) if ((int)jget_num(s, "id", 0) == atoi(k->string)) there = 1;
                if (!there) cJSON_Delete(cJSON_DetachItemViaPointer(sec, k));
                k = next;
            }
        }
        state_unlock();
    }
    cJSON_Delete(list);
    return n;
}

typedef struct {
    int uploaded, downloaded, conflicts, skipped, failed, states_up, states_down, noop, deferred, cards, cards_down;
    int unreachable; /* failed because the server never answered */
    char error[256];
} sync_result;

/* A game played by several emulators that keep its save in the same format
 * (save_format) has one save: the newest of their copies goes to the others,
 * the copies replaced backed up first. Runs before the saves are scanned (so
 * one save goes up) and after the downloads (so a new one reaches them all). */
static int share_bundles(scan_ctx *c);

static int share_saves(scan_ctx *c) {
    int copied = share_bundles(c);
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *a = &c->roms[i];
        if (!a->map->save_format[0] || a->map->own_slot || !a->rules.sync_saves || a->map->save_layout[0]) continue;
        /* The first member of each group does the work. */
        int first = 1;
        const char *family = profiles_save_family(a->map->save_format);
        for (int k = 0; k < i && first; k++)
            if (c->roms[k].rom_id == a->rom_id && !strcmp(profiles_save_family(c->roms[k].map->save_format), family) &&
                !c->roms[k].map->own_slot && c->roms[k].rules.sync_saves)
                first = 0;
        if (!first) continue;
        char paths[8][PATH_MAX_LEN];
        const local_rom *whose[8];
        int n = 0, newest = -1, newest_blank = 0, newest_fresh = 0;
        time_t newest_t = 0;
        for (int k = i; k < c->nroms && n < 8; k++) {
            const local_rom *r = &c->roms[k];
            if (r->rom_id != a->rom_id || strcmp(profiles_save_family(r->map->save_format), family) || r->map->own_slot ||
                !r->rules.sync_saves)
                continue;
            download_target(r, "", c->slot, c, paths[n], sizeof paths[n]);
            time_t t = file_mtime(paths[n]);
            /* A blank card (an emulator made a fresh one) never beats one with
             * saves; then the save RomM just gave beats the others. */
            const int blank = psx_card_saves(r->map, paths[n]) == 0, fresh = is_fresh(paths[n]);
            if (t > 0 && (newest < 0 || (newest_blank && !blank) ||
                          (blank == newest_blank && (fresh != newest_fresh ? fresh : t > newest_t))))
                newest_t = t, newest = n, newest_blank = blank, newest_fresh = fresh;
            whose[n] = r;
            n++;
        }
        if (n < 2 || newest < 0) continue;
        char want[33];
        if (save_md5(paths[newest], want) != 0) continue;
        for (int k = 0; k < n; k++) {
            char have[33] = "", tmp[PATH_MAX_LEN];
            if (k == newest || (save_md5(paths[k], have) == 0 && !strcmp(have, want))) continue;
            /* In the form this one keeps it (DeSmuME's footer on or off). */
            snprintf(tmp, sizeof tmp, "%s/tmp/shared-%d.sav", plat_data_dir(), a->rom_id);
            if (mkdir_parent(tmp) != 0 || copy_file(paths[newest], tmp) != 0 ||
                fit_save(whose[k]->map->save_format, tmp, paths[k]) != 0) {
                unlink(tmp);
                continue;
            }
            if (file_exists(paths[k])) backup_file(c, paths[k], a->rom_id);
            mkdir_parent(paths[k]);
            if (move_file(tmp, paths[k]) == 0) {
                set_file_mtime(paths[k], newest_t);
                if (newest_fresh) mark_fresh(paths[k]); /* RomM's, there too */
                LOGI("shared save: %s -> %s", paths[newest], paths[k]);
                note_shared(whose[k]->map, 0);
                drop_resume(c, whose[k]);
                copied++;
            }
        }
    }
    return copied;
}

/* The same for a GameCube game two Dolphins play (GCI folders): the game's
 * .gci files from the one that saved last go to the other's folder. */
static int share_bundles(scan_ctx *c) {
    int copied = 0;
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *a = &c->roms[i];
        if (strcmp(a->map->save_layout, "gci") != 0 || a->map->own_slot || !a->rules.sync_saves) continue;
        int first = 1;
        for (int k = 0; k < i && first; k++)
            if (c->roms[k].rom_id == a->rom_id && !strcmp(c->roms[k].map->save_layout, "gci") &&
                !c->roms[k].map->own_slot && c->roms[k].rules.sync_saves)
                first = 0;
        char id[24];
        if (!first || rom_game_id(a, id, sizeof id) != 0) continue;
        bundle bs[4];
        const local_rom *whose[4];
        int n = 0, newest = -1;
        for (int k = i; k < c->nroms && n < 4; k++) {
            const local_rom *r = &c->roms[k];
            if (r->rom_id != a->rom_id || strcmp(r->map->save_layout, "gci") || r->map->own_slot || !r->rules.sync_saves)
                continue;
            if (bundle_for_rom(r, id, &bs[n]) != 0) continue;
            whose[n] = r;
            if (bs[n].n && (newest < 0 || bs[n].mtime > bs[newest].mtime)) newest = n;
            n++;
        }
        char want[33] = "";
        if (n >= 2 && newest >= 0) bundle_hash(&bs[newest], want);
        for (int k = 0; want[0] && k < n; k++) {
            char have[33];
            bundle_hash(&bs[k], have);
            if (k == newest || !strcmp(have, want)) continue;
            if (bs[k].n) { /* what was there, kept as a zip */
                char cur[PATH_MAX_LEN];
                snprintf(cur, sizeof cur, "%s/tmp/%s-shared.zip", plat_data_dir(), id);
                if (bundle_zip(&bs[k], cur) == 0) backup_file(c, cur, a->rom_id);
                unlink(cur);
            }
            for (int f = 0; f < bs[k].n; f++) unlink(bs[k].files[f].path);
            mkdir_p(bs[k].dir);
            for (int f = 0; f < bs[newest].n; f++) {
                char to[PATH_MAX_LEN];
                path_join(to, sizeof to, bs[k].dir, bs[newest].files[f].name);
                if (copy_file(bs[newest].files[f].path, to) == 0) set_file_mtime(to, file_mtime(bs[newest].files[f].path));
            }
            LOGI("shared save: %s's GameCube saves -> %s", id, bs[k].dir);
            note_shared(whose[k]->map, 0);
            drop_resume(c, whose[k]);
            copied++;
        }
        for (int k = 0; k < n; k++) bundle_free(&bs[k]);
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
    static const char *const sections[] = {"roms", "saves", "states", "conflicts", "memcards", "too_big"};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        cJSON *sec = state_section(sections[i]);
        while (sec->child) cJSON_Delete(cJSON_DetachItemViaPointer(sec, sec->child));
    }
    cJSON_DeleteItemFromObjectCaseSensitive(state_section("server"), "upload_limit");
    cJSON_DeleteItemFromObjectCaseSensitive(state_section("server"), "upload_limit_at");
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

/* Mupen64Plus standalone's N64 saves (n64.h). A .srm newer than its
 * files (from RomM or another emulator) goes out to them, not while it runs:
 * it reads them as a game starts and writes them over as it saves. Then files
 * it saved since the last look go into the .srm the sync sees. */
static void n64_split_pass(const scan_ctx *c) {
    for (int i = 0; i < c->nroms; i++) {
        const local_rom *r = &c->roms[i];
        const profile_map *m = r->map;
        if (!m->n64_split_dir[0] || !r->rom_id || !r->rules.sync_saves || !r->n64_md5[0]) continue;
        char file[PATH_MAX_LEN], name[256], shadow[PATH_MAX_LEN];
        if (game_file(r, file, sizeof file) != 0) continue;
        if (n64_mupen_name(m->n64_split_dir, m->n64_ini, file, r->n64_md5, name, sizeof name) != 0) {
            LOGD("%s: no name for its save (no %s)", path_basename(file), m->n64_ini);
            continue;
        }
        download_target(r, "", c->slot, c, shadow, sizeof shadow);
        if (emulator_closed(m) && n64_unfold(m->n64_split_dir, name, shadow, is_fresh(shadow)) > 0)
            LOGI("%s's save is in %s's files now (%s)", path_basename(file), emu_name(m), name);
        if (!is_fresh(shadow) && n64_fold(m->n64_split_dir, name, shadow) > 0) LOGD("%s: %s's files joined into %s", name, emu_name(m), shadow);
    }
}

/* only_rom limits the transfers to one game; the rest wait for the next sync. */
static void run_sync(const char *reason, int only_rom, sync_result *res) {
    memset(res, 0, sizeof *res);
    g_shared_to[0] = 0;
    g_shared = g_shared_bundles = 0;
    forget_other_server();
    scan_ctx c;
    set_phase("scanning local files");
    if (scan_prepare(&c) != 0) {
        snprintf(res->error, sizeof res->error, "%s",
                 c.server_status == 0 && http_out_of_memory() ? "out of memory" : "cannot reach RomM server");
        res->unreachable = c.server_status == 0;
        scan_free(&c);
        return;
    }
    romm_update_device_name();
    n64_split_pass(&c);
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
        res->unreachable = st == 0;
        cJSON_Delete(neg);
        scan_free(&c);
        return;
    }
    int session_id = (int)jget_num(neg, "session_id", 0);
    /* A running game would overwrite a downloaded save, so downloads wait until
     * it closes. The sync right after launch still downloads. */
    int upload_only = g_game_running && strcmp(reason, "game started") != 0 && !only_rom;

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
        if (only_rom && rom_id != only_rom) continue;
        if (!our_slot(&c, slot)) continue; /* another device's slot: its saves stay its own */
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
        if (want_up && f && save_id > 0 && server_hash[0] && strcmp(server_hash, f->hash) != 0) {
            /* The same trap the other way round: a file this device never synced,
             * against a save RomM already has. Its timestamp says nothing (a fresh
             * install, or an emulator's starter NVRAM, is "newer" than years of
             * progress), so it doesn't get to replace the server's on a hunch.
             * One saved while we watched its emulator run is progress: it goes. */
            state_lock();
            const int unknown = state_entry("saves", f->path) == NULL;
            state_unlock();
            if (unknown && !saved_in_last_run(f->map, f->mtime)) {
                is_conflict = 1;
                want_up = 0;
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

        if (want_up && f && refused_as_too_big(f)) {
            LOGD("%s: still too big for RomM, not sent", f->path);
            res->skipped++; /* said when it was refused */
            continue;
        }
        if (want_up && f) {
            long ust = 0;
            if (do_upload(&c, f, session_id, overwrite, &ust) == 0) {
                res->uploaded++;
                if (last_rom != rom_id) roms_touched++;
                last_rom = rom_id;
                events_add("save", "uploaded", rom_id, f->path, emu_name(f->map), overwrite ? "Kept this console's" : NULL);
            } else if (ust == 409) {
                /* The server copy changed since negotiate. */
                record_conflict(f, op);
                res->conflicts++;
            } else if (ust == 413) {
                say_too_big(f);
                res->skipped++;
            } else {
                res->failed++;
                events_add("save", "failed", rom_id, f->path, emu_name(f->map), "Upload failed");
            }
        } else if (want_down && upload_only) {
            res->deferred++;
        } else if (want_down) {
            const local_rom *r = f ? f->rom : find_rom(&c, rom_id, jget_str(op, "emulator", NULL));
            if (!r) {
                res->skipped++; /* game isn't on this console */
                continue;
            }
            if (!strcmp(r->map->save_layout, "xbox") && !emulator_closed(r->map)) {
                res->deferred++; /* not onto a disk xemu has open */
                continue;
            }
            char gid[24];
            if (r->map->save_layout[0] && rom_game_id(r, gid, sizeof gid) != 0) {
                say_unreadable(r); /* once; failing again every sync mends nothing */
                res->skipped++;
                continue;
            }
            char dest[PATH_MAX_LEN];
            int ok;
            const char *why = "Download failed";
            if (r->map->save_layout[0]) {
                const int rc = do_download_bundle(&c, save_id, r, slot, session_id, jget_str(op, "server_content_hash", ""), 1);
                if (rc == -3) {
                    res->skipped++; /* came down before and wasn't the game's (said then) */
                    continue;
                }
                if (rc == -2) why = "RomM's save doesn't match this game here";
                ok = rc == 0;
            } else {
                if (f)
                    str_copy(dest, sizeof dest, f->path);
                else
                    download_target(r, jget_str(op, "file_name", ""), slot, &c, dest, sizeof dest);
                int rc = do_download(&c, save_id, dest, f, r, rom_id, slot, session_id, jget_str(op, "server_content_hash", ""),
                                     jget_str(op, "server_updated_at", ""), 1);
                if (rc == -2) {
                    record_conflict(f, op);
                    res->conflicts++;
                    events_add("save", "conflict", rom_id, dest, emu_name(r->map), "RomM's card is blank");
                    continue;
                }
                ok = rc == 0;
            }
            const char *where = r->map->save_layout[0] ? r->path : dest;
            if (ok) {
                res->downloaded++;
                events_add("save", "downloaded", rom_id, where, emu_name(r->map), is_conflict ? "Kept RomM's" : NULL);
            } else {
                res->failed++;
                events_add("save", "failed", rom_id, where, emu_name(r->map), why);
            }
        } else {
            res->skipped++;
        }
    }
    /* A save changed since its last sync that RomM gave no answer for stays
     * as it is: say so, or nobody ever knows why it didn't go up. */
    for (int i = 0; i < c.nsaves; i++) {
        const local_file *f = &c.saves[i];
        if (only_rom && f->rom_id != only_rom) continue;
        int answered = 0;
        cJSON_ArrayForEach(op, cJSON_GetObjectItemCaseSensitive(neg, "operations"))
            if ((int)jget_num(op, "rom_id", 0) == f->rom_id && !strcmp(jget_str(op, "slot", ""), f->slot)) answered = 1;
        if (answered) continue;
        state_lock();
        const cJSON *e = state_entry("saves", f->path);
        const int changed = !e || strcmp(jget_str(e, "hash", ""), f->hash) != 0;
        state_unlock();
        if (changed)
            LOGW("%s changed since its last sync, but RomM gave no answer for it (game %d, slot %s)", f->path, f->rom_id,
                 f->slot);
    }
    /* What went up just now changed RomM's lists: asked again from here. */
    cJSON_Delete(c.romm_saves);
    c.romm_saves = NULL;
    /* The games RomM has saves of: as negotiate said, those uploaded now,
     * and those synced here before. */
    if (!only_rom) {
        c.romm_has = cJSON_CreateObject();
        cJSON_ArrayForEach(op, cJSON_GetObjectItemCaseSensitive(neg, "operations")) {
            char key[16];
            snprintf(key, sizeof key, "%d", (int)jget_num(op, "rom_id", 0));
            if (!cJSON_GetObjectItemCaseSensitive(c.romm_has, key)) cJSON_AddTrueToObject(c.romm_has, key);
        }
        for (int i = 0; i < c.nsaves; i++) {
            char key[16];
            snprintf(key, sizeof key, "%d", c.saves[i].rom_id);
            if (!cJSON_GetObjectItemCaseSensitive(c.romm_has, key)) cJSON_AddTrueToObject(c.romm_has, key);
        }
        /* Negotiate leaves out what this device already synced and RomM
         * hasn't changed since: those games are known from the records. */
        state_lock();
        const cJSON *rec;
        cJSON_ArrayForEach(rec, state_section("saves")) {
            char key[16];
            snprintf(key, sizeof key, "%d", (int)jget_num(rec, "rom_id", 0));
            if (!cJSON_GetObjectItemCaseSensitive(c.romm_has, key)) cJSON_AddTrueToObject(c.romm_has, key);
        }
        state_unlock();
    }
    cJSON_Delete(neg);
    if (!upload_only) {
        set_phase("restoring missing saves");
        res->downloaded += restore_missing_saves(&c, only_rom);
        res->downloaded += restore_missing_bundles(&c, only_rom);
        set_phase("sharing saves between emulators");
        res->downloaded += share_bundle_saves(&c, only_rom);
        /* Saves put on RomM without a slot (its web page): looked for at
         * startup, by hand and every so often; not after a game closes or a
         * save changes here, which are about this console's own saves and
         * should be quick. */
        if (strcmp(reason, "game exited") != 0 && strcmp(reason, "save changed") != 0) {
            set_phase("saves from other devices");
            res->downloaded += fetch_slotless_saves(&c, only_rom);
        }
    }

    set_phase("finishing");
    cJSON *done = cJSON_CreateObject();
    cJSON_AddNumberToObject(done, "operations_completed", res->uploaded + res->downloaded + res->noop);
    cJSON_AddNumberToObject(done, "operations_failed", res->failed);
    pthread_mutex_lock(&g_req_lock);
    cJSON *ps = only_rom ? NULL : g_play_sessions;
    if (!only_rom) g_play_sessions = NULL;
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
        if (do_states == 2 && !upload_only) res->states_down = sync_states_down(&c, only_rom);
        res->states_up = sync_states_up(&c, only_rom);
    }

    if (!only_rom) res->cards = memcard_sync(upload_only, &res->cards_down);
    /* A save that just came down reaches the other emulators that share it. */
    share_saves(&c);
    n64_split_pass(&c);

    /* BIOS files last: a first sync can bring hundreds of MB of them (Switch
     * firmware), and saves shouldn't wait on that. Not while a game runs: its
     * emulator may be reading the folder. */
    if (!only_rom && !upload_only) {
        set_phase("BIOS files");
        firmware_sync(c.maps, c.nmaps, c.platforms, 0, NULL);
    }

    state_lock();
    state_save();
    state_unlock();
    scan_free(&c);
}

/* Runs a sync and adds it to the history. */
static void run_and_record(const char *reason, int only_rom, sync_result *r) {
    pthread_mutex_lock(&g_run_lock);
    events_set_sync(reason);
    g_running = 1;
    run_sync(reason, only_rom, r);
    g_running = 0;
    g_last_run = time(NULL);
    set_phase("idle");
    pthread_mutex_unlock(&g_run_lock);
    events_save();

    cJSON *h = cJSON_CreateObject();
    g_sync_count++;
    cJSON_AddNumberToObject(h, "time", (double)g_last_run);
    cJSON_AddStringToObject(h, "reason", reason);
    cJSON_AddNumberToObject(h, "uploaded", r->uploaded);
    cJSON_AddNumberToObject(h, "downloaded", r->downloaded);
    cJSON_AddNumberToObject(h, "conflicts", r->conflicts);
    cJSON_AddNumberToObject(h, "skipped", r->skipped);
    cJSON_AddNumberToObject(h, "failed", r->failed);
    cJSON_AddNumberToObject(h, "states_uploaded", r->states_up);
    cJSON_AddNumberToObject(h, "states_downloaded", r->states_down);
    cJSON_AddNumberToObject(h, "deferred", r->deferred);
    cJSON_AddNumberToObject(h, "cards_uploaded", r->cards);
    cJSON_AddNumberToObject(h, "cards_downloaded", r->cards_down);
    if (r->error[0]) cJSON_AddStringToObject(h, "error", r->error);
    state_lock();
    state_history_add(h);
    state_save();
    state_unlock();
}

static int sync_summary(const sync_result *r, char *out, size_t n);

static int notify_on(void) {
    config_lock();
    int notify = g_cfg.notify;
    config_unlock();
    return notify;
}

/* The console can lose the server for a few minutes while the rest
 * of the LAN reaches it fine, and the sync a game launch asked for was then
 * gone until the next periodic one. A sync the server never answered is
 * tried again after 1, 2 and 4 minutes, quietly; only giving up gets a toast.
 * The retry only uploads while a game runs, like any sync but the launch one. */
#define RETRY_FIRST_SEC 60
#define MAX_RETRIES 3
/* XPSemu takes a while to go after it leaves the screen: its disk is read
 * again every 20 s, for 2 minutes, once nothing's in front. */
#define DISK_RETRY_SEC 20
#define MAX_DISK_RETRIES 6

static volatile time_t g_retry_at;
static int g_retries, g_disk_retries;

int sync_retry_due(void) {
    time_t at = g_retry_at;
    if (!at || time(NULL) < at) return 0;
    g_retry_at = 0;
    return 1;
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
        /* Only manual syncs run until setup is finished, and none at all
         * with save sync off (RomM is then only where games come from). */
        int ready, on;
        config_lock();
        ready = g_cfg.setup_complete;
        on = g_cfg.save_sync;
        config_unlock();
        if (!on) {
            LOGI("sync (%s) skipped: save sync is off", reason);
            continue;
        }
        if (!ready && strcmp(reason, "manual") != 0 && strcmp(reason, "first sync") != 0) {
            LOGI("sync (%s) skipped: setup not finished", reason);
            continue;
        }
        sync_result r;
        const unsigned mark = events_seq();
        g_disk_busy = 0;
        run_and_record(reason, 0, &r);

        int retry_in = 0;
        if (r.error[0] && r.unreachable && g_retries < MAX_RETRIES) {
            retry_in = RETRY_FIRST_SEC << g_retries++;
            g_retry_at = time(NULL) + retry_in;
        } else if (g_disk_busy && !g_game_running && g_disk_retries < MAX_DISK_RETRIES) {
            g_disk_retries++;
            g_retry_at = time(NULL) + DISK_RETRY_SEC;
            LOGI("Xbox saves wait for XPSemu to finish closing: looking again in %ds", DISK_RETRY_SEC);
        } else {
            g_retries = 0;
            if (!g_disk_busy || g_game_running) g_disk_retries = 0;
            g_retry_at = 0;
        }

        int notify = notify_on();
        if (r.error[0] && retry_in) {
            LOGE("sync failed: %s; trying again in %ds", r.error, retry_in);
        } else if (r.error[0]) {
            LOGE("sync failed: %s", r.error);
            if (notify) plat_notify("RommPS: couldn't sync\n%s", r.unreachable ? "Check your connection" : r.error);
        } else {
            /* Issue #10 - Added notification and log file for memory card operations. */
            LOGI("sync done: %d up, %d down, %d conflicts, %d failed, %d states, %d cards up, %d cards down; largest free block %d MB",
                 r.uploaded, r.downloaded, r.conflicts, r.failed, r.states_up, r.cards, r.cards_down, mem_largest_mb());
            char text[200], games[400];
            if (notify && sync_summary(&r, text, sizeof text)) {
                if (events_games(mark, 3, games, sizeof games)) plat_notify("RommPS: %s\n%s", games, text);
                else plat_notify("RommPS: %s", text);
            }
        }
    }
    return NULL;
}

/* What a sync did, for its notification: "2 saves up, 1 state down, 1
 * conflict to resolve". 0 when it did nothing worth telling. */
static int sync_summary(const sync_result *r, char *out, size_t n) {
    /* Saves handed from one emulator to another aren't from RomM: said as
     * where they went. */
    const int down = r->downloaded - g_shared_bundles;
    struct { int count; const char *one, *many; } parts[] = {
        {r->uploaded, "save up", "saves up"},       {down, "save down", "saves down"},
        {r->states_up, "state up", "states up"},    {r->states_down, "state down", "states down"},
        {r->cards, "memory card up", "memory cards up"},       {r->cards_down, "memory card down", "memory cards down"},
        {r->conflicts, "conflict to resolve", "conflicts to resolve"}, {r->failed, "failed", "failed"}};
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0] && o < n; i++)
        if (parts[i].count > 0)
            o += (size_t)snprintf(out + o, n - o, "%s%d %s", o ? ", " : "", parts[i].count,
                                  parts[i].count == 1 ? parts[i].one : parts[i].many);
    if (g_shared && g_shared_to[0] && o < n) {
        /* "now in RetroArch and Crux2" */
        char names[200];
        str_copy(names, sizeof names, g_shared_to);
        int count = 1;
        for (char *p = names; *p; p++) count += *p == '|';
        o += (size_t)snprintf(out + o, n - o, "%snow in ", o ? ", " : "");
        int k = 0;
        char *save = NULL;
        for (char *t = strtok_r(names, "|", &save); t && o < n; t = strtok_r(NULL, "|", &save), k++)
            o += (size_t)snprintf(out + o, n - o, "%s%s", k == 0 ? "" : k == count - 1 ? " and " : ", ", t);
    }
    return o > 0;
}

void sync_init(void) {
    thread_start(worker, NULL);
}

static void plural(char *out, size_t n, int count, const char *what) {
    snprintf(out, n, "%d %s%s", count, what, count == 1 ? "" : "s");
}

void download_notice(char *out, size_t n, const char *name, const char *system) {
    if (system && system[0]) snprintf(out, n, "%s\nDownloaded for %s", name, system);
    else snprintf(out, n, "%s\nDownloaded", name);
}

int sync_game(int rom_id, const char *name, const char *system, const char *problem, sync_game_result *out) {
    memset(out, 0, sizeof *out);
    config_lock();
    int ready = g_cfg.setup_complete && g_cfg.save_sync;
    config_unlock();
    if (!config_is_paired() || !ready) return -1;

    sync_result r;
    run_and_record("game downloaded", rom_id, &r);
    out->saves = r.uploaded + r.downloaded;
    out->states = r.states_up + r.states_down;
    out->conflicts = r.conflicts;
    out->failed = r.failed || r.error[0];
    LOGI("saves for %s: %d saves, %d states, %d conflicts%s", name, out->saves, out->states, out->conflicts,
         out->failed ? ", some failed" : "");
    if (!notify_on()) return 0;
    char head[400];
    download_notice(head, sizeof head, name, system);
    if (problem && problem[0]) {
        plat_notify("RommPS: %s, but %s", head, problem);
    } else if (out->failed) {
        plat_notify("RommPS: %s, but its saves couldn't sync", head);
    } else if (out->conflicts) {
        plat_notify("RommPS: %s, with a save conflict to resolve", head);
    } else if (out->saves || out->states) {
        char saves[32] = "", states[32] = "";
        if (out->saves) plural(saves, sizeof saves, out->saves, "save");
        if (out->states) plural(states, sizeof states, out->states, "state");
        plat_notify("RommPS: %s, with %s%s%s", head, saves, out->saves && out->states ? " and " : "", states);
    } else {
        plat_notify("RommPS: %s", head);
    }
    return 0;
}

void sync_request(const char *reason) {
    pthread_mutex_lock(&g_req_lock);
    if (!g_req_reason[0]) str_copy(g_req_reason, sizeof g_req_reason, reason);
    pthread_cond_signal(&g_req_cond);
    pthread_mutex_unlock(&g_req_lock);
}

int sync_is_running(void) { return g_running; }

void sync_pause(void) { pthread_mutex_lock(&g_run_lock); }
void sync_resume(void) { pthread_mutex_unlock(&g_run_lock); }
time_t sync_last_run(void) { return g_last_run; }

void sync_add_play_session(time_t start, time_t end, const char *title_id) {
    /* The emulator's last run, for saves made in it (saved_in_last_run). */
    if (title_id && title_id[0]) {
        state_lock();
        cJSON *run = state_entry_ensure("runs", title_id);
        jset_num(run, "start", (double)start);
        jset_num(run, "end", (double)end);
        state_save();
        state_unlock();
    }
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
        if (rc && st == 413) snprintf(err, (size_t)en, "the save is too big for RomM");
        else if (rc) snprintf(err, (size_t)en, "upload failed (HTTP %ld)", st);
    } else if (f->is_bundle) {
        rc = do_download_bundle(&c, (int)jget_num(e, "save_id", 0), f->rom, f->slot, 0, jget_str(e, "server_hash", ""), 1);
        if (rc) snprintf(err, (size_t)en, "download failed");
    } else {
        rc = do_download(&c, (int)jget_num(e, "save_id", 0), path, f, f->rom, f->rom_id, f->slot, 0, jget_str(e, "server_hash", ""),
                         jget_str(e, "server_updated_at", ""), 0);
        if (rc) snprintf(err, (size_t)en, "download failed");
    }
    if (rc == 0) {
        state_lock();
        state_entry_remove("conflicts", path);
        state_save();
        state_unlock();
        /* The save kept reaches the other emulators of the game now, before
         * an older copy of theirs comes back as a new conflict. */
        if (!f->is_bundle) {
            mark_fresh(path);
            share_saves(&c);
            n64_split_pass(&c);
        }
        events_set_sync("conflict resolved");
        events_add("save", "resolved", f->rom_id, path, emu_name(f->map), local ? "Kept this console's" : "Kept RomM's");
        events_save();
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
        if (!our_slot(&c, slot)) continue;
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
    const local_rom *r = is_state ? find_rom_for_state(&c, rom_id, jget_str(meta, "emulator", ""))
                                  : find_rom(&c, rom_id, jget_str(meta, "emulator", NULL));
    if (!r && is_state && find_rom(&c, rom_id, NULL)) {
        snprintf(err, (size_t)en, "made by %s, which this console's emulator can't load",
                 jget_str(meta, "emulator", "")[0] ? jget_str(meta, "emulator", "") : "an unknown emulator");
        goto done;
    }
    if (!r) {
        if (!why_no_sync(&c, rom_id, is_state, err, en)) snprintf(err, (size_t)en, "the game isn't on this console");
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
        rc = fetch_state(&c, meta, dest, r);
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
        /* In the form of the emulator whose file it goes into. */
        const local_rom *into = f ? f->rom : r;
        rc = into && fit_save(into->map->save_format, tmp, dest) != 0 ? -1 : move_file(tmp, dest);
        if (rc) {
            unlink(tmp);
            snprintf(err, (size_t)en, "could not write %s", dest);
        } else {
            drop_resume(&c, into);
        }
        /* Not recorded as synced: the next sync uploads it as the newest version. */
    }
    if (rc == 0) {
        LOGI("restored %s %d to %s", kind, id, dest);
        events_add(is_state ? "state" : "save", "restored", rom_id, dest, r ? emu_name(r->map) : "", "Older version");
    }
done:
    scan_free(&c);
    pthread_mutex_unlock(&g_run_lock);
    cJSON_Delete(meta);
    if (rc == 0) sync_request("restore");
    return rc;
}

/* One tracked file for sync_tracked. */
static void tracked_file(cJSON *games, const cJSON *roms, const profile_map *maps, int nmaps, const char *kind,
                         const cJSON *e, const cJSON *conflicts) {
    int rom_id = (int)jget_num(e, "rom_id", 0);
    /* The game's record, one whose file is here first. */
    const cJSON *game = NULL, *g;
    int here = 0;
    cJSON_ArrayForEach(g, roms) if ((int)jget_num(g, "rom_id", -1) == rom_id && rom_id) {
        const int exists = file_exists(g->string) || dir_exists(g->string);
        if (!game || (exists && !here)) game = g;
        here |= exists;
    }
    const char *platform = game ? jget_str(game, "platform_dir", "") : "";
    const char *profile = game ? jget_str(game, "profile", "") : "";
    const char *emulator = "";
    for (int i = 0; i < nmaps; i++)
        if (!strcmp(maps[i].profile_id, profile) && (!platform[0] || !strcmp(maps[i].platform_dir, platform)))
            emulator = maps[i].profile_name;
    /* A record from an older version names its profile differently: by platform. */
    for (int i = 0; i < nmaps && !emulator[0] && platform[0]; i++)
        if (!strcmp(maps[i].platform_dir, platform)) emulator = maps[i].profile_name;

    char key[16];
    snprintf(key, sizeof key, "%d", rom_id);
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(games, key);
    if (!entry) {
        entry = cJSON_AddObjectToObject(games, key);
        cJSON_AddNumberToObject(entry, "rom_id", rom_id);
        cJSON_AddStringToObject(entry, "name", game ? jget_str(game, "name", "") : "");
        cJSON_AddStringToObject(entry, "platform", platform);
        cJSON_AddStringToObject(entry, "emulator", emulator);
        cJSON_AddArrayToObject(entry, "files");
    }
    const char *path = e->string;
    const char *status = "synced";
    if (cJSON_GetObjectItemCaseSensitive(conflicts, path)) {
        status = "conflict";
    } else if (!here) { /* the game isn't on this console any more */
        status = "no_game";
    } else if (strchr(path, '#')) { /* a save folder synced as a zip */
        status = "synced";
    } else if (!file_exists(path)) {
        status = "missing";
    } else {
        char ns[24];
        snprintf(ns, sizeof ns, "%lld", (long long)file_mtime_ns(path));
        /* A newer time with the same content (emulators rewrite saves) is still in sync. */
        char hash[33] = "";
        if ((strcmp(ns, jget_str(e, "mtime_ns", "")) != 0 || file_size(path) != (int64_t)jget_num(e, "size", -1)) &&
            (save_md5(path, hash) != 0 || strcmp(hash, jget_str(e, "hash", "")) != 0))
            status = "changed";
    }
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "kind", kind);
    cJSON_AddStringToObject(f, "path", path);
    cJSON_AddStringToObject(f, "status", status);
    cJSON_AddNumberToObject(f, "synced_at", jget_num(e, "synced_at", 0));
    cJSON_AddNumberToObject(f, "size", jget_num(e, "size", 0));
    if (!strcmp(kind, "save")) {
        cJSON_AddStringToObject(f, "slot", jget_str(e, "slot", ""));
        cJSON_AddNumberToObject(f, "server_id", jget_num(e, "save_id", 0));
    } else {
        cJSON_AddNumberToObject(f, "server_id", jget_num(e, "state_id", 0));
        cJSON_AddBoolToObject(f, "from_here", jget_num(e, "mine", 0) != 0);
    }
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(entry, "files"), f);
}

cJSON *sync_tracked(int only_rom) {
    profile_map *maps = NULL;
    config_lock();
    int nmaps = profiles_expand(g_cfg.profiles, &maps);
    config_unlock();
    cJSON *games = cJSON_CreateObject();
    state_lock();
    const cJSON *roms = state_section("roms"), *conflicts = state_section("conflicts"), *e;
    static const char *const kinds[][2] = {{"saves", "save"}, {"states", "state"}};
    for (int k = 0; k < 2; k++)
        cJSON_ArrayForEach(e, state_section(kinds[k][0])) {
            if (only_rom && (int)jget_num(e, "rom_id", 0) != only_rom) continue;
            tracked_file(games, roms, maps, nmaps, kinds[k][1], e, conflicts);
        }
    state_unlock();
    profiles_free(maps);

    /* {"platforms": [{"platform": "gba", "games": [...]}], "saves": n, "states": n, "conflicts": n} */
    cJSON *out = cJSON_CreateObject(), *plats = cJSON_AddArrayToObject(out, "platforms"), *gm;
    int n[3] = {0, 0, 0};
    while ((gm = games->child)) {
        cJSON_DetachItemViaPointer(games, gm);
        const char *pl = jget_str(gm, "platform", "");
        cJSON *p = NULL, *x;
        cJSON_ArrayForEach(x, plats) if (!strcmp(jget_str(x, "platform", ""), pl)) p = x;
        if (!p) {
            p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "platform", pl);
            cJSON_AddArrayToObject(p, "games");
            cJSON_AddItemToArray(plats, p);
        }
        cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(gm, "files")) {
            n[!strcmp(jget_str(x, "kind", ""), "state")]++;
            if (!strcmp(jget_str(x, "status", ""), "conflict")) n[2]++;
        }
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(p, "games"), gm);
    }
    cJSON_Delete(games);
    cJSON_AddNumberToObject(out, "saves", n[0]);
    cJSON_AddNumberToObject(out, "states", n[1]);
    cJSON_AddNumberToObject(out, "conflicts", n[2]);
    return out;
}
