#include "firmware.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "events.h"
#include "platform.h"
#include "romm.h"
#include "state.h"
#include "util.h"

/* PS2 BIOS, the USA v2.30 first: PCSX2 takes the first one it finds, so only
 * one goes in. */
#define PS2_PICK \
    "[\"21038400dc633070a78ad53090c53017\",\"dc69f0643a3030aaa4797501b483d6c4\",\"80ac46fa7e77b8ab4366e86948e54f83\"," \
    "\"93ea3bcee4252627919175ff1b16a1d9\",\"d3e81e95db25f5a86a7b7474550a2155\"]"

/* RetroArch cores, by the core name in their .info file: profiles detected
 * before the rules existed don't carry them. From the cores' .info files. */
static const struct {
    const char *core;
    const char *rules;
} CORE_RULES[] = {
    {"mGBA", "{\"names\":{\"32fbbd84168d3482956eb3c5051637f5\":\"gb_bios.bin\",\"dbfce9db9deaa2567f6a84fde55f9680\":\"gbc_bios.bin\","
             "\"d574d4f9c12f305074798f54c091a8b4\":\"sgb_bios.bin\"},\"from\":{\"super-gb\":[\"sgb_bios.bin\",\"sgb1.boot.rom\"]}}"},
    {"Snes9x", "{\"from\":{\"satellaview\":[\"BS-X.bin\"],\"sufami-turbo\":[\"STBIOS.bin\"]}}"},
    {"Mupen64Plus-Next", "{\"only_named\":true,\"names\":{\"8d3d9f294b6e174bc7b1d2fd1c727530\":\"Mupen64plus/IPL.n64\"},"
                         "\"from\":{\"64dd\":[\"*\"]}}"},
    {"FinalBurn Neo", "{\"dir\":\"{bios}/fbneo\"}"},
    {"LRPS2", "{\"pick_one\":" PS2_PICK "}"},
    {"PCSX2", "{\"pick_one\":" PS2_PICK "}"},
    {"PS5SX2", "{\"pick_one\":" PS2_PICK "}"},
    {"PPSSPP", "{\"skip\":true}"},
    {"Dolphin", "{\"dir\":\"{bios}/dolphin-emu/Sys/GC\",\"only_named\":true,\"names\":{"
                "\"b17148254a5799684c7d783206504926\":\"USA/IPL.bin\",\"fc924a7c879b661abc37cec4f018fdf3\":\"USA/IPL.bin\","
                "\"019e39822a9ca3029124f74dd4d55ac4\":\"USA/IPL.bin\",\"db92574caab77a7ec99d4605fd6f2450\":\"EUR/IPL.bin\","
                "\"339848a0b7c2124cf155276c1e79cbd0\":\"EUR/IPL.bin\",\"0cdda509e2da83c85bfe423dd87346cc\":\"EUR/IPL.bin\"}}"},
    {"Flycast", "{\"dir\":\"{bios}/dc\",\"names\":{\"e10c53c2f8b90bab96ead2d368858623\":\"dc_boot.bin\","
                "\"0a93f7940c455905bea6e392dfde92a4\":\"dc_flash.bin\"},"
                "\"from\":{\"arcade\":[\"naomi.zip\",\"naomi2.zip\",\"awbios.zip\"]}}"},
    {"Azahar", "{\"dir\":\"{save_dir}/Azahar/sysdata\",\"only_files\":[\"aes_keys.txt\",\"seeddb.bin\"]}"},
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static cJSON *rules_for(const profile_map *m) {
    config_lock();
    cJSON *r = profile_firmware_rules(g_cfg.profiles, m);
    config_unlock();
    if (r || !m->core_name[0]) return r;
    for (size_t i = 0; i < sizeof CORE_RULES / sizeof CORE_RULES[0]; i++)
        if (str_ieq(CORE_RULES[i].core, m->core_name)) return cJSON_Parse(CORE_RULES[i].rules);
    return NULL;
}

static void target_dir(const profile_map *m, const cJSON *rules, char *out, size_t n) {
    const char *tpl = jget_str(rules, "dir", NULL);
    if (!tpl) {
        if (m->firmware_subdir[0]) path_join(out, n, m->bios_dir, m->firmware_subdir);
        else str_copy(out, n, m->bios_dir);
        return;
    }
    const char *vars[] = {"bios", m->bios_dir, "root", m->root, "rom_dir", m->rom_dir, "save_dir", m->save_dir, NULL};
    str_template(out, n, tpl, vars);
}

int firmware_find(const profile_map *m, const char *name, char *out, size_t n) {
    cJSON *rules = rules_for(m);
    char dir[PATH_MAX_LEN], sub[PATH_MAX_LEN] = "";
    target_dir(m, rules, dir, sizeof dir);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(rules, "by_ext"))
        if (cJSON_IsString(e) && str_ieq(path_ext(name), e->string)) str_copy(sub, sizeof sub, e->valuestring);
    cJSON_Delete(rules);
    out[0] = 0;
    if (sub[0] == '/') { /* a folder of its own */
        str_copy(dir, sizeof dir, sub);
    } else if (sub[0] && !strchr(sub, '{') && !strstr(sub, "..")) {
        char in[PATH_MAX_LEN];
        path_join(in, sizeof in, dir, sub);
        str_copy(dir, sizeof dir, in);
    }
    if (!dir[0]) return -1;
    path_join(out, n, dir, name);
    return file_exists(out) ? 0 : -1;
}

/* A file RomM really has, with a name that can't leave the folder. */
static int usable(const cJSON *f) {
    const char *name = jget_str(f, "file_name", "");
    return name[0] && name[0] != '.' && !strchr(name, '/') && !strchr(name, '\\') &&
           !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "missing_from_fs")) && jget_num(f, "id", 0) > 0;
}

/* The platform's BIOS files: in the platform itself on RomM 5, else asked for. */
static cJSON *platform_firmware(const cJSON *plat) {
    const cJSON *fw = cJSON_GetObjectItemCaseSensitive(plat, "firmware");
    cJSON *list = NULL;
    if (cJSON_IsArray(fw)) {
        list = cJSON_Duplicate(fw, 1);
    } else if (jget_num(plat, "firmware_count", 0) <= 0) {
        return cJSON_CreateArray();
    } else {
        char path[64];
        long st = 0;
        snprintf(path, sizeof path, "/api/firmware?platform_id=%d", (int)jget_num(plat, "id", 0));
        list = romm_call("GET", path, NULL, &st);
        if (st != 200 || !cJSON_IsArray(list)) {
            LOGW("BIOS files for %s: RomM said %ld", jget_str(plat, "slug", "?"), st);
            cJSON_Delete(list);
            return NULL;
        }
    }
    cJSON *f;
    cJSON_ArrayForEach(f, list) if (!cJSON_GetObjectItemCaseSensitive(f, "platform_id"))
        cJSON_AddNumberToObject(f, "platform_id", jget_num(plat, "id", 0));
    return list;
}

/* The platform's files plus the ones the rules take from other platforms
 * ("from": {"arcade": ["naomi.zip"]}, "*" for all): RomM keeps some BIOS
 * under a platform of their own (64dd, satellaview) or another (Naomi's under
 * arcade, played by Dreamcast emulators). */
static cJSON *with_borrowed(const cJSON *files, const cJSON *rules, const cJSON *platforms, int *fails) {
    cJSON *out = cJSON_Duplicate(files, 1);
    const cJSON *from = cJSON_GetObjectItemCaseSensitive(rules, "from"), *src, *plat;
    cJSON_ArrayForEach(src, from) {
        cJSON_ArrayForEach(plat, platforms) {
            if (!str_ieq(jget_str(plat, "slug", ""), src->string) && !str_ieq(jget_str(plat, "fs_slug", ""), src->string)) continue;
            cJSON *theirs = platform_firmware(plat);
            if (!theirs) (*fails)++;
            const cJSON *f;
            cJSON_ArrayForEach(f, theirs) {
                const char *name = jget_str(f, "file_name", "");
                const cJSON *x;
                int take = 0;
                cJSON_ArrayForEach(x, src) take |= cJSON_IsString(x) && (!strcmp(x->valuestring, "*") || str_ieq(x->valuestring, name));
                if (take) cJSON_AddItemToArray(out, cJSON_Duplicate(f, 1));
            }
            cJSON_Delete(theirs);
            break;
        }
    }
    return out;
}

static const cJSON *by_md5(const cJSON *files, const char *md5) {
    const cJSON *f;
    cJSON_ArrayForEach(f, files) if (usable(f) && str_ieq(jget_str(f, "md5_hash", ""), md5)) return f;
    return NULL;
}

typedef struct {
    const cJSON *f;
    char name[512]; /* under the target folder, or a path of its own (by_ext's "/...") */
    int named;      /* the name came from the rules' "names" */
} plan_t;

/* Where a planned file goes: its name under dir, or the path by_ext gave it. */
static void plan_dest(const char *dir, const char *name, char *out, size_t n) {
    if (name[0] == '/') str_copy(out, n, name);
    else path_join(out, n, dir, name);
}

/* A Switch firmware set alone is 200-odd .nca files. */
#define MAX_PLAN 1024

static int planned_name(const plan_t *p, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(p[i].name, name)) return 1;
    return 0;
}

static int planned_file(const plan_t *p, int n, const cJSON *f) {
    for (int i = 0; i < n; i++)
        if (p[i].f == f) return 1;
    return 0;
}

static int list_has_name(const cJSON *arr, const char *name) {
    const cJSON *x;
    cJSON_ArrayForEach(x, arr) if (cJSON_IsString(x) && str_ieq(x->valuestring, name)) return 1;
    return 0;
}

/* A whole BIOS for an emulator that takes the first one it finds (PS2: 4 MB).
 * The dumps' companions (.nvm, .mec, rom1, erom) are smaller, and are never
 * the one to keep or the proof there's one already. */
#define BIOS_IMAGE_MIN (4 << 20)

static int bios_image(const cJSON *files, int64_t size) {
    const cJSON *f;
    if (size < BIOS_IMAGE_MIN) return 0;
    cJSON_ArrayForEach(f, files) if (usable(f) && size == (int64_t)jget_num(f, "file_size_bytes", -1)) return 1;
    return 0;
}

/* An emulator that takes the first BIOS it finds already has one: a file of
 * the size of one of RomM's. */
static int has_one_already(const char *dir, const cJSON *files) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int found = 0;
    while (d && !found && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX_LEN];
        path_join(p, sizeof p, dir, de->d_name);
        found = bios_image(files, file_size(p));
    }
    if (d) closedir(d);
    return found;
}

static int same_stem_file(const char *dir, const char *name, const char *ext, char *out, size_t n) {
    char stem[256];
    path_stem(stem, sizeof stem, name);
    char file[300];
    snprintf(file, sizeof file, "%s%s", stem, ext);
    path_join(out, n, dir, file);
    return file_exists(out);
}

/* Once per folder, for an emulator that takes the first BIOS it finds: of
 * the files there that are RomM's (by md5), the one used last stays (PCSX2
 * keeps a BIOS's settings in <bios>.nvm beside it), else the preferred one;
 * the others go. Their settings files stay: they're the user's, and they do
 * nothing without the BIOS. Files RomM doesn't have are the user's too. */
static void tidy_pick_one(const char *dir, const cJSON *files, const cJSON *pick) {
    state_lock();
    const int done = state_entry("firmware_tidy", dir) != NULL;
    state_unlock();
    if (done || !dir_exists(dir)) return;
    DIR *d = opendir(dir);
    struct dirent *de;
    char names[256][256];
    int nn = 0, keep = -1;
    long long keep_rank = 1LL << 62;
    while (d && nn < 256 && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX_LEN], md5[33];
        path_join(p, sizeof p, dir, de->d_name);
        if (!bios_image(files, file_size(p)) || md5_file_hex(p, md5) != 0 || !by_md5(files, md5)) continue;
        char nvm[PATH_MAX_LEN];
        long long r = 1000 + nn;
        const cJSON *x;
        int k = 0;
        cJSON_ArrayForEach(x, pick) {
            if (cJSON_IsString(x) && str_ieq(x->valuestring, md5)) r = 100 + k;
            k++;
        }
        /* Used: PCSX2 left its settings beside it; the most recently used wins. */
        struct stat st;
        if (same_stem_file(dir, de->d_name, ".nvm", nvm, sizeof nvm) && stat(nvm, &st) == 0) r = -(long long)st.st_mtime;
        str_copy(names[nn], sizeof names[nn], de->d_name);
        if (r < keep_rank) {
            keep_rank = r;
            keep = nn;
        }
        nn++;
    }
    if (d) closedir(d);
    int removed = 0;
    for (int i = 0; i < nn; i++) {
        if (i == keep) continue;
        char p[PATH_MAX_LEN];
        path_join(p, sizeof p, dir, names[i]);
        if (unlink(p) != 0) continue;
        state_lock();
        cJSON_DeleteItemFromObjectCaseSensitive(state_section("firmware"), p);
        state_unlock();
        events_add("bios", "removed", 0, p, NULL, names[i]);
        removed++;
    }
    state_lock();
    jset_num(state_entry_ensure("firmware_tidy", dir), "removed", removed);
    state_save();
    state_unlock();
    if (removed) {
        LOGI("BIOS: kept %s in %s, removed %d more of RomM's", names[keep], dir, removed);
        plat_notify("%s: %d extra BIOS file%s removed\nKept %s", plat_product(), removed, removed == 1 ? "" : "s", names[keep]);
    }
}

static int still_ours(const char *path, const char *rec_md5, int64_t rec_size);

/* A Switch firmware folder is one set, matched to the keys beside it: files of
 * two versions together fail ProsperoEden's check ("NCA validation failed",
 * code 21) for every game. RomM's set goes only into a folder with none of the
 * user's; RomM's files already put in beside the user's set come out again.
 * 1 when the folder holds the user's own set. */
#define ONE_SET_EXTS ".nca"

/* The folder a set's file is in: its own, or for Ryujinx's layout, where
 * each NCA is a folder of its own (registered/<id>.nca/00), the one above. */
static void set_folder(const char *path, char *out, size_t n) {
    str_copy(out, n, path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        out[0] = 0;
        return;
    }
    *slash = 0;
    if (ext_in_list(path_basename(out), ONE_SET_EXTS) && (slash = strrchr(out, '/'))) *slash = 0;
}

/* Whether path is one of a set's files in folder: folder/<x>.nca, or
 * folder/<x>.nca/<file> (Ryujinx's). */
static int in_set(const char *path, const char *folder) {
    const size_t fl = strlen(folder);
    if (strncmp(path, folder, fl) != 0 || path[fl] != '/') return 0;
    const char *name = path + fl + 1, *slash = strchr(name, '/');
    if (!slash) return ext_in_list(name, ONE_SET_EXTS);
    char first[512];
    snprintf(first, sizeof first, "%.*s", (int)(slash - name), name);
    return !strchr(slash + 1, '/') && ext_in_list(first, ONE_SET_EXTS);
}

/* One of our set's files gone, and its own folder when it had one (Ryujinx's <id>.nca/). */
static int unlink_set_file(const char *path) {
    if (unlink(path) != 0) return -1;
    char up[PATH_MAX_LEN];
    str_copy(up, sizeof up, path);
    char *slash = strrchr(up, '/');
    if (slash) *slash = 0;
    if (ext_in_list(path_basename(up), ONE_SET_EXTS)) rmdir(up);
    return 0;
}

static int users_set(const char *folder, const char *ext) {
    DIR *d = opendir(folder);
    struct dirent *de;
    int users = 0, removed = 0;
    char paths[64][PATH_MAX_LEN];
    int nours = 0;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.' || !str_ieq(path_ext(de->d_name), ext)) continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, folder, de->d_name);
        state_lock();
        int ours = state_entry("firmware", path) != NULL;
        const size_t pl = strlen(path);
        const cJSON *rec;
        cJSON_ArrayForEach(rec, state_section("firmware")) ours |= !strncmp(rec->string, path, pl) && rec->string[pl] == '/';
        state_unlock();
        if (!ours) users = 1;
    }
    if (d) closedir(d);
    if (!users) return 0;
    /* In batches: the folder changes as files go. */
    do {
        nours = 0;
        state_lock();
        const cJSON *recs = state_section("firmware"), *e;
        cJSON_ArrayForEach(e, recs) {
            if (nours < 64 && in_set(e->string, folder)) str_copy(paths[nours++], sizeof paths[0], e->string);
        }
        state_unlock();
        for (int i = 0; i < nours; i++) {
            char md5[33];
            state_lock();
            const cJSON *rec = state_entry("firmware", paths[i]);
            str_copy(md5, sizeof md5, jget_str(rec, "md5", ""));
            const int64_t size = rec ? (int64_t)jget_num(rec, "size", -1) : -1;
            state_unlock();
            /* Changed since we put it there: the user's now, left as it is. */
            if (file_exists(paths[i]) && still_ours(paths[i], md5, size) && unlink_set_file(paths[i]) == 0) removed++;
            state_lock();
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("firmware"), paths[i]);
            state_unlock();
        }
    } while (nours == 64);
    if (removed) {
        state_lock();
        state_save();
        state_unlock();
        LOGI("BIOS: %s holds a firmware set of its own: removed the %d files of RomM's put in beside it", folder, removed);
        plat_notify("%s: removed RomM's Switch firmware\nYour own firmware was there already", plat_product());
    }
    return 1;
}

/* Which of the platform's files go to this emulator, under which names. */
static int make_plan(const cJSON *rules, const cJSON *files, const char *dir, plan_t *plan) {
    int n = 0;
    const cJSON *pick = cJSON_GetObjectItemCaseSensitive(rules, "pick_one");
    if (cJSON_IsArray(pick)) {
        tidy_pick_one(dir, files, pick);
        if (has_one_already(dir, files)) return 0;
        const cJSON *best = NULL, *x, *f;
        cJSON_ArrayForEach(x, pick) if (!best && cJSON_IsString(x)) best = by_md5(files, x->valuestring);
        cJSON_ArrayForEach(f, files) if (!best && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "is_verified")) &&
                                         bios_image(files, (int64_t)jget_num(f, "file_size_bytes", -1))) best = f;
        if (best) {
            plan[0].f = best;
            plan[0].named = 0;
            str_copy(plan[0].name, sizeof plan[0].name, jget_str(best, "file_name", ""));
            n = 1;
        }
        return n;
    }
    /* Named ones first, in the order listed: the first one RomM has takes the name. */
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(rules, "names"), *nm;
    cJSON_ArrayForEach(nm, names) {
        const cJSON *f = cJSON_IsString(nm) ? by_md5(files, nm->string) : NULL;
        if (!f || n == MAX_PLAN || planned_name(plan, n, nm->valuestring) || planned_file(plan, n, f) ||
            strstr(nm->valuestring, "..") || nm->valuestring[0] == '/')
            continue;
        plan[n].f = f;
        str_copy(plan[n].name, sizeof plan[n].name, nm->valuestring);
        plan[n].named = 1;
        n++;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rules, "only_named"))) return n;
    int left_out = 0;
    const cJSON *only = cJSON_GetObjectItemCaseSensitive(rules, "only_files");
    const cJSON *by_ext = cJSON_GetObjectItemCaseSensitive(rules, "by_ext");
    const int named = n;
    int sets_checked = 0; /* ONE_SET_EXTS: 1 RomM's set goes in, 2 the user's is there */
    const cJSON *f;
    cJSON_ArrayForEach(f, files) {
        if (!usable(f) || planned_file(plan, n, f)) continue;
        /* RomM keeps some files twice (dc_flash.bin and flash.bin): one that
         * already goes in under the name the emulator wants is enough. */
        int dup = 0;
        for (int k = 0; k < named && !dup; k++) dup = str_ieq(jget_str(plan[k].f, "md5_hash", "-"), jget_str(f, "md5_hash", ""));
        if (dup) continue;
        const char *name = jget_str(f, "file_name", "");
        if (cJSON_IsArray(only) && !list_has_name(only, name)) continue;
        char full[512];
        str_copy(full, sizeof full, name);
        if (cJSON_IsObject(by_ext)) {
            /* A folder under dir, or one of its own ("/..."): Eden's keys
             * beside its data, its firmware in its NAND, wherever that is.
             * Or a path with the file's id in it ("{id}": its name up to the
             * first dot), as Ryujinx keeps an NCA: registered/<id>.nca/00. */
            const cJSON *sub = NULL, *e;
            cJSON_ArrayForEach(e, by_ext) if (cJSON_IsString(e) && str_ieq(path_ext(name), e->string)) sub = e;
            const char *v = sub ? sub->valuestring : "", *idp = strstr(v, "{id}");
            if (!sub || strstr(v, "..") || (strchr(v, '{') && (!idp || strchr(idp + 1, '{')))) continue;
            if (idp) snprintf(full, sizeof full, "%.*s%.*s%s", (int)(idp - v), v, (int)strcspn(name, "."), name, idp + 4);
            else if (v[0]) snprintf(full, sizeof full, "%s/%s", v, name);
        }
        if (ext_in_list(name, ONE_SET_EXTS)) {
            if (sets_checked == 0) {
                char at[PATH_MAX_LEN], in[PATH_MAX_LEN];
                plan_dest(dir, full, at, sizeof at);
                set_folder(at, in, sizeof in);
                if (!in[0]) str_copy(in, sizeof in, dir);
                sets_checked = users_set(in, path_ext(name)) ? 2 : 1;
            }
            if (sets_checked == 2) continue;
        }
        if (planned_name(plan, n, full)) continue;
        if (n == MAX_PLAN) {
            left_out++;
            continue;
        }
        plan[n].f = f;
        str_copy(plan[n].name, sizeof plan[n].name, full);
        plan[n].named = 0;
        n++;
    }
    if (left_out) LOGW("BIOS for %s: %d more files than the %d a sync takes, left out", dir, left_out, MAX_PLAN);
    return n;
}

typedef struct {
    char md5[33];
    char path[PATH_MAX_LEN];
} fetched_t;

/* Whether a file we put in is still the one we put there. */
static int still_ours(const char *path, const char *rec_md5, int64_t rec_size) {
    char have[33];
    if (rec_size >= 0 && file_size(path) != rec_size) return 0;
    return rec_md5[0] && md5_file_hex(path, have) == 0 && str_ieq(have, rec_md5);
}

/* Puts one file in place. 1 when it was added, 0 when nothing was needed, -1
 * when it failed. Downloads each file once a run; other emulators get a copy. */
static int place(const profile_map *m, const cJSON *f, const char *dest, fetched_t *got, int *ngot, int maxgot) {
    const char *md5 = jget_str(f, "md5_hash", "");
    const int64_t size = (int64_t)jget_num(f, "file_size_bytes", -1);
    if (file_exists(dest) || dir_exists(dest)) {
        char rec_md5[33];
        state_lock();
        const cJSON *rec = state_entry("firmware", dest);
        const int ours = rec != NULL;
        str_copy(rec_md5, sizeof rec_md5, jget_str(rec, "md5", ""));
        const int64_t rec_size = rec ? (int64_t)jget_num(rec, "size", -1) : -1;
        state_unlock();
        if (!ours) return 0; /* the user's (or the emulator's) own */
        if (str_ieq(rec_md5, md5) && file_size(dest) == size) return 0; /* up to date */
        if (!still_ours(dest, rec_md5, rec_size)) {
            /* Changed since we put it there: the user's now, never touched again. */
            LOGI("BIOS %s was changed here, left as it is", dest);
            state_lock();
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("firmware"), dest);
            state_save();
            state_unlock();
            return 0;
        }
        unlink(dest);
    }
    if (mkdir_parent(dest) != 0) {
        LOGW("BIOS %s: can't make its folder", dest);
        return -1;
    }
    int ok = 0;
    for (int i = 0; i < *ngot && !ok; i++)
        if (md5[0] && !strcmp(got[i].md5, md5) && file_exists(got[i].path)) ok = link_or_copy(got[i].path, dest) == 0;
    if (!ok) {
        char path[512];
        http_resp r;
        char *q = http_escape(jget_str(f, "file_name", ""));
        snprintf(path, sizeof path, "/api/firmware/%d/content/%s", (int)jget_num(f, "id", 0), q);
        free(q);
        ok = romm_download(path, dest, NULL, NULL, &r) == 0;
        if (!ok) LOGW("BIOS %s didn't download: %s", jget_str(f, "file_name", ""), r.error);
    }
    char have[33] = "";
    if (ok && md5[0] && (md5_file_hex(dest, have) != 0 || !str_ieq(have, md5))) {
        LOGW("BIOS %s came down damaged (md5 %s, RomM has %s)", dest, have, md5);
        ok = 0;
    }
    if (!ok) {
        unlink(dest);
        return -1;
    }
    if (*ngot < maxgot) {
        str_copy(got[*ngot].md5, sizeof got[*ngot].md5, md5);
        str_copy(got[*ngot].path, sizeof got[*ngot].path, dest);
        (*ngot)++;
    }
    state_lock();
    cJSON *e = state_entry_ensure("firmware", dest);
    jset_str(e, "md5", md5);
    jset_num(e, "size", (double)file_size(dest));
    jset_num(e, "platform_id", jget_num(f, "platform_id", 0));
    state_save();
    state_unlock();
    LOGI("BIOS for %s: %s", m->profile_name, dest);
    events_add("bios", "downloaded", 0, dest, m->profile_name, path_basename(dest));
    return 1;
}

/* Whether the plan's file goes to the old set's place (by_ext's "/..." or under dir). */
static int planned_path(const char *dir, const plan_t *plan, int np, const char *path) {
    for (int k = 0; k < np; k++) {
        char planned[PATH_MAX_LEN];
        plan_dest(dir, plan[k].name, planned, sizeof planned);
        if (!strcmp(planned, path)) return 1;
    }
    return 0;
}

/* RomM's firmware set swapped for another version: the files of ours that
 * the new set doesn't have go, or the two versions sit in one folder and
 * Eden fails every game on "NCA validation failed". The plan only holds set
 * files when RomM's set goes in (never beside the user's own, see users_set),
 * and a file changed since we put it there is the user's and stays.
 * Only once the new set is all here (whole: every file of it RomM listed came
 * down, checked, in a list as long as the platform says): a set half
 * downloaded, or a list cut short, would cost the old set for one that can't
 * run either, and 200-odd downloads to put it back. A new set less than half
 * the size of what it replaces is taken for a list cut short too. */
static void drop_old_set(const char *dir, const plan_t *plan, int np, const cJSON *files, int whole) {
    char folder[PATH_MAX_LEN] = "";
    int planned = 0;
    for (int k = 0; k < np; k++) {
        if (!ext_in_list(jget_str(plan[k].f, "file_name", ""), ONE_SET_EXTS)) continue;
        planned++;
        if (folder[0]) continue;
        char at[PATH_MAX_LEN];
        plan_dest(dir, plan[k].name, at, sizeof at);
        set_folder(at, folder, sizeof folder);
    }
    if (!folder[0]) return;
    const cJSON *f;
    cJSON_ArrayForEach(f, files) if (ext_in_list(jget_str(f, "file_name", ""), ONE_SET_EXTS) && !usable(f)) whole = 0;
    int leaving = 0;
    state_lock();
    const cJSON *rec;
    cJSON_ArrayForEach(rec, state_section("firmware")) leaving += in_set(rec->string, folder) && !planned_path(dir, plan, np, rec->string);
    state_unlock();
    if (!leaving) return;
    if (!whole || planned * 2 < leaving) {
        LOGW("BIOS: RomM's firmware set in %s isn't all here (%d files of it, %d of the old one): the old one stays until it is",
             folder, planned, leaving);
        return;
    }
    int removed = 0;
    state_lock();
    cJSON *recs = state_section("firmware"), *e = recs ? recs->child : NULL;
    while (e) {
        cJSON *next = e->next;
        const char *p = e->string;
        if (in_set(p, folder) && !planned_path(dir, plan, np, p)) {
            if (file_exists(p) && still_ours(p, jget_str(e, "md5", ""), (int64_t)jget_num(e, "size", -1)) && unlink_set_file(p) == 0) {
                events_add("bios", "removed", 0, p, NULL, path_basename(p));
                removed++;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(recs, p);
        }
        e = next;
    }
    if (removed) state_save();
    state_unlock();
    if (removed) LOGI("BIOS: RomM's firmware set changed, removed %d files of the old one from %s", removed, folder);
}

/* Copies of ours under RomM's other name for a file that now goes in under
 * the name the emulator wants (dc/flash.bin beside dc/dc_flash.bin) go,
 * unless they were changed since. */
static void drop_named_twins(const char *dir, const plan_t *plan, int np) {
    for (int k = 0; k < np; k++) {
        if (!plan[k].named) continue;
        const char *md5 = jget_str(plan[k].f, "md5_hash", "");
        char keep[PATH_MAX_LEN];
        plan_dest(dir, plan[k].name, keep, sizeof keep);
        if (!md5[0]) continue;
        state_lock();
        cJSON *recs = state_section("firmware"), *e = recs ? recs->child : NULL;
        while (e) {
            cJSON *next = e->next;
            size_t dl = strlen(dir);
            const int here = !strncmp(e->string, dir, dl) && e->string[dl] == '/' && !strchr(e->string + dl + 1, '/');
            if (here && strcmp(e->string, keep) != 0 && str_ieq(jget_str(e, "md5", ""), md5) &&
                still_ours(e->string, md5, (int64_t)jget_num(e, "size", -1))) {
                int planned = 0;
                for (int j = 0; j < np && !planned; j++) {
                    char p[PATH_MAX_LEN];
                    plan_dest(dir, plan[j].name, p, sizeof p);
                    planned = !strcmp(p, e->string);
                }
                if (!planned) {
                    LOGI("BIOS: removed %s, the same file as %s", e->string, keep);
                    events_add("bios", "removed", 0, e->string, NULL, path_basename(e->string));
                    unlink(e->string);
                    cJSON_DeleteItemFromObjectCaseSensitive(recs, e->string);
                    state_save();
                }
            }
            e = next;
        }
        state_unlock();
    }
}

/* The first file in dir with one of by_ext's extensions (what RomM brought
 * for an installer, PS3UPDAT.PUP), or "". */
static void delivered(const char *dir, const cJSON *by_ext, char *out, size_t n) {
    out[0] = 0;
    DIR *d = cJSON_IsObject(by_ext) ? opendir(dir) : NULL;
    struct dirent *de;
    while (d && !out[0] && (de = readdir(d))) {
        const cJSON *e;
        cJSON_ArrayForEach(e, by_ext) if (!out[0] && de->d_name[0] != '.' && str_ieq(path_ext(de->d_name), e->string))
            path_join(out, n, dir, de->d_name);
    }
    if (d) closedir(d);
}

/* Firmware an emulator installs itself, not one copied into place (RPCS3
 * and the PS3's PS3UPDAT.PUP): "installed" names what's there once it's
 * done. RomM's installer file comes down like any BIOS file (by_ext, into
 * dir), and "install" says what to do with it, {file} being where it is;
 * "install_without" says what to do while RomM has none. Missing, that's
 * said: whenever one of its platform's games comes down (asked), and once a
 * run otherwise. */
static void say_install(const profile_map *m, const cJSON *rules, const char *dir, int asked) {
    const char *tpl = jget_str(rules, "installed", "");
    if (!tpl[0]) return;
    char path[PATH_MAX_LEN], file[PATH_MAX_LEN], how[PATH_MAX_LEN * 2];
    const char *vars[] = {"bios", m->bios_dir, "root", m->root, "rom_dir", m->rom_dir, "save_dir", m->save_dir, NULL};
    str_template(path, sizeof path, tpl, vars);
    if (file_exists(path) || dir_exists(path)) return;
    delivered(dir, cJSON_GetObjectItemCaseSensitive(rules, "by_ext"), file, sizeof file);
    if (file[0]) {
        const char *with[] = {"file", file, NULL};
        str_template(how, sizeof how, jget_str(rules, "install", ""), with);
    } else {
        str_copy(how, sizeof how, jget_str(rules, "install_without", jget_str(rules, "install", "")));
    }
    /* Once a run for what there is to say: the file coming down is news. */
    static char said[16][PATH_MAX_LEN];
    static int nsaid;
    char what[PATH_MAX_LEN];
    snprintf(what, sizeof what, "%s|%s", m->profile_id, file);
    int before = 0;
    for (int i = 0; i < nsaid && !before; i++) before = !strcmp(said[i], what);
    if (before && !asked) return;
    if (!before && nsaid < 16) str_copy(said[nsaid++], sizeof said[0], what);
    LOGW("%s: its firmware isn't installed (no %s)", m->profile_name, path);
    config_lock();
    const int notify = g_cfg.notify;
    config_unlock();
    if (notify) plat_notify("%s: %s needs its firmware installed%s%s", plat_product(), m->profile_name, how[0] ? "\n" : "", how);
}

int firmware_sync(const profile_map *maps, int n, const cJSON *platforms, int only_platform, int *failed) {
    int added = 0, fails = 0;
    cJSON *own = NULL;
    if (!platforms) {
        long st = 0;
        own = romm_call("GET", "/api/platforms", NULL, &st);
        if (st != 200 || !cJSON_IsArray(own)) {
            cJSON_Delete(own);
            if (failed) *failed = 0;
            return -1;
        }
        platforms = own;
    }
    pthread_mutex_lock(&g_lock);
    fetched_t *got = calloc(64, sizeof *got);
    int ngot = 0;
    plan_t *plan = calloc(MAX_PLAN, sizeof *plan);
    const cJSON *plat;
    cJSON_ArrayForEach(plat, platforms) {
        if (!got || !plan) break;
        const int pid = (int)jget_num(plat, "id", 0);
        if (only_platform && pid != only_platform) continue;
        const char *slug = jget_str(plat, "slug", ""), *fs_slug = jget_str(plat, "fs_slug", "");
        int wanted = 0;
        for (int i = 0; i < n && !wanted; i++) wanted = profile_map_handles(&maps[i], slug, fs_slug);
        if (!wanted) continue;
        cJSON *files = platform_firmware(plat);
        if (!files) {
            fails++;
            continue;
        }
        /* Shorter than the platform says: cut off on the way, not a smaller set. */
        const int whole_list = cJSON_GetArraySize(files) >= (int)jget_num(plat, "firmware_count", 0);
        for (int i = 0; i < n; i++) {
            const profile_map *m = &maps[i];
            if (!profile_map_handles(m, slug, fs_slug)) continue;
            cJSON *rules = rules_for(m);
            char dir[PATH_MAX_LEN];
            target_dir(m, rules, dir, sizeof dir);
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rules, "skip"))) {
                say_install(m, rules, dir, only_platform != 0);
                cJSON_Delete(rules);
                continue;
            }
            const int fails_before = fails;
            cJSON *mine = with_borrowed(files, rules, platforms, &fails);
            const int np = dir[0] && cJSON_GetArraySize(mine) > 0 ? make_plan(rules, mine, dir, plan) : 0;
            int whole = whole_list && fails == fails_before; /* no borrowed list missing either */
            for (int k = 0; k < np; k++) {
                char dest[PATH_MAX_LEN];
                plan_dest(dir, plan[k].name, dest, sizeof dest);
                const int r = place(m, plan[k].f, dest, got, &ngot, 64);
                if (r > 0) added++;
                else if (r < 0) fails++;
                if (ext_in_list(jget_str(plan[k].f, "file_name", ""), ONE_SET_EXTS) && (r < 0 || (!file_exists(dest) && !dir_exists(dest))))
                    whole = 0;
            }
            drop_named_twins(dir, plan, np);
            drop_old_set(dir, plan, np, mine, whole);
            say_install(m, rules, dir, only_platform != 0); /* with what just came down */
            cJSON_Delete(mine);
            cJSON_Delete(rules);
        }
        cJSON_Delete(files);
    }
    free(got);
    free(plan);
    pthread_mutex_unlock(&g_lock);
    cJSON_Delete(own);
    events_save();
    if (added) plat_notify("%s: %d BIOS file%s added", plat_product(), added, added == 1 ? "" : "s");
    if (failed) *failed = fails;
    return added;
}
