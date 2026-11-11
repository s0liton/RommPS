/* Finds emulators in the folders plat_info() names: homebrew/ on internal
 * storage and USB drives on the PS5, /data/retroarch on the PS4. For RetroArch
 * the platform list comes from the installed cores. Each core's .info file gives
 * "corename", the folder name for sorted saves, and "database", its systems.
 * The PS5 keeps both in <root>/cores; the PS4 has cores in /data/self/retroarch/cores
 * and every core's .info in /data/retroarch/info, as retroarch.cfg says.
 *
 * Standalone emulators come from the console's catalog (plat_info, e.g.
 * platform/ps4/emulators.json): one is offered when its app is installed or
 * one of its folders exists. Each entry is
 *   {"id", "name", "title_ids": [...], "detect": [paths], "profile": {...}}
 * where the profile uses the system keys of DB_MAP below for its platforms. */
#include "detect.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "profiles.h"
#include "ra2.h"
#include "state.h"
#include "util.h"
#include "presets.h"

/* Bump this when generated profiles gain new keys, or existing installs keep
 * their old profile forever (ask me how I know). */
#define PROFILE_GENERATION 3

/* libretro database names to RomM slugs (the first is the folder name), and
 * the system's ES-DE name: PS5 RetroArch 3.0 and newer's folder for it. */
static const struct {
    const char *db;
    const char *slugs;
    const char *esde;
} DB_MAP[] = {
    {"Nintendo - Nintendo Entertainment System", "nes,famicom", "nes"},
    {"Nintendo - Family Computer Disk System", "fds", "fds"},
    {"Nintendo - Super Nintendo Entertainment System", "snes,sfam", "snes"},
    {"Nintendo - Satellaview", "satellaview", "satellaview"},
    {"Nintendo - Game Boy", "gb", "gb"},
    {"Nintendo - Game Boy Color", "gbc", "gbc"},
    {"Nintendo - Game Boy Advance", "gba", "gba"},
    {"Nintendo - Nintendo 64", "n64", "n64"},
    {"Nintendo - Nintendo DS", "nds", "nds"},
    {"Nintendo - Virtual Boy", "virtualboy", "virtualboy"},
    {"Nintendo - GameCube", "ngc,gamecube", "gc"},
    {"Nintendo - Wii", "wii", "wii"},
    {"Sega - Mega Drive - Genesis", "genesis,genesis-slash-megadrive,megadrive", "genesis"},
    {"Sega - Master System - Mark III", "sms,mastersystem", "mastersystem"},
    {"Sega - Game Gear", "gamegear,gg", "gamegear"},
    {"Sega - Mega-CD - Sega CD", "segacd,sega-cd", "segacd"},
    {"Sega - SG-1000", "sg1000", "sg-1000"},
    {"Sega - 32X", "sega32,sega-32x", "sega32x"},
    {"Sega - Saturn", "saturn", "saturn"},
    {"Sega - Dreamcast", "dc,dreamcast", "dreamcast"},
    {"Sony - PlayStation", "psx,ps1", "psx"},
    {"Sony - PlayStation 2", "ps2", "ps2"},
    {"Sony - PlayStation Portable", "psp", "psp"},
    {"FBNeo - Arcade Games", "arcade,fbneo,neogeoaes,neogeomvs,cps1,cps2,cps3", "fbneo"},
    {"MAME", "arcade,mame", "mame"},
    {"NEC - PC Engine - TurboGrafx 16", "tg16,turbografx16--1,pce,pcengine", "pcengine"},
    {"NEC - PC Engine CD - TurboGrafx-CD", "turbografx-cd,pcecd", "pcenginecd"},
    {"SNK - Neo Geo Pocket", "neo-geo-pocket,ngp", "ngp"},
    {"SNK - Neo Geo Pocket Color", "neo-geo-pocket-color,ngpc", "ngpc"},
    {"Bandai - WonderSwan", "wonderswan,ws", "wonderswan"},
    {"Bandai - WonderSwan Color", "wonderswan-color,wsc", "wonderswancolor"},
    {"Atari - 2600", "atari2600", "atari2600"},
    {"Atari - 7800", "atari7800", "atari7800"},
    {"Atari - Lynx", "lynx", "atarilynx"},
    {"Commodore - 64", "c64", "c64"},
    {"Commodore - Amiga", "amiga", "amiga"},
    {"DOS", "dos", "dos"},
    {"Nintendo - Nintendo 3DS", "3ds", "n3ds"},
    {"Nintendo - Pokemon Mini", "pokemon-mini", "pokemini"},
    {"Nintendo - Sufami Turbo", "sufami-turbo", "sufami"},
    {"Nintendo - Wii (Digital)", "wii", "wii"},
    {"NEC - PC Engine SuperGrafx", "supergrafx", "supergrafx"},
    {"NEC - PC-FX", "pc-fx", "pcfx"},
    {"SNK - Neo Geo CD", "neo-geo-cd", "neogeocd"},
    {"The 3DO Company - 3DO", "3do", "3do"},
    {"Atari - 5200", "atari5200", "atari5200"},
    {"Atari - Jaguar", "jaguar", "atarijaguar"},
    {"Sega - PICO", "sega-pico", "pico"},
    {"Commodore - CD32", "amiga-cd32", "amigacd32"},
    {"Commodore - CDTV", "commodore-cdtv", "cdtv"},
    {"ScummVM", "scummvm", "scummvm"},
};

const char *detect_esde_name(const cJSON *entry) {
    const char *db = detect_db_name(entry);
    for (size_t i = 0; db && i < sizeof DB_MAP / sizeof DB_MAP[0]; i++)
        if (!strcmp(DB_MAP[i].db, db)) return DB_MAP[i].esde;
    return NULL;
}

const char *detect_db_name(const cJSON *entry) {
    const char *db = jget_str(entry, "db", NULL);
    if (db) return db;
    if (str_ieq(jget_str(entry, "core_name", ""), "MAME")) return "MAME"; /* arcade, like FBNeo */
    const cJSON *slug;
    cJSON_ArrayForEach(slug, cJSON_GetObjectItemCaseSensitive(entry, "romm")) {
        if (!cJSON_IsString(slug)) continue;
        for (size_t i = 0; i < sizeof DB_MAP / sizeof DB_MAP[0]; i++) {
            char slugs[256], *save = NULL;
            str_copy(slugs, sizeof slugs, DB_MAP[i].slugs);
            for (char *t = strtok_r(slugs, ",", &save); t; t = strtok_r(NULL, ",", &save))
                if (str_ieq(t, slug->valuestring)) return DB_MAP[i].db;
        }
    }
    return NULL;
}

/* Value of key = "value" in a libretro .info file. */
static int info_get(const char *txt, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *l = txt; l && *l;) {
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, key, kl) && (l[kl] == ' ' || l[kl] == '=')) {
            const char *q = strchr(l, '"'), *eol = strchr(l, '\n');
            if (q && (!eol || q < eol)) {
                const char *e = strchr(q + 1, '"');
                if (e) {
                    size_t len = (size_t)(e - q - 1);
                    if (len >= n) len = n - 1;
                    memcpy(out, q + 1, len);
                    out[len] = 0;
                    return 1;
                }
            }
        }
        l = strchr(l, '\n');
        if (l) l++;
    }
    return 0;
}

static cJSON *slug_array(const char *csv) {
    cJSON *a = cJSON_CreateArray();
    char buf[256];
    str_copy(buf, sizeof buf, csv);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        cJSON_AddItemToArray(a, cJSON_CreateString(t));
    return a;
}

/* What a core needs beyond the defaults, from platform/retroarch_cores.json:
 * the first rule whose names (or "contains") and systems match adds its
 * "entry" keys to the core's platform entry. */
#include "retroarch_cores_json.h" /* generated from platform/retroarch_cores.json */

static int name_in(const cJSON *list, const char *name, int contains) {
    const cJSON *n;
    cJSON_ArrayForEach(n, list)
        if (cJSON_IsString(n) && (contains ? strstr(name, n->valuestring) != NULL : str_ieq(name, n->valuestring))) return 1;
    return 0;
}

/* Keys a core's rule decides even in a saved profile: how its memory cards
 * sync follows the core, as an emulator's follows its preset (profiles.c). */
static const char *const RULE_ONLY[] = {"memcards", "cards", "save_layout", "pcsx2_cards"};

/* Adds the matching rule's keys to e: all of them, or (missing_only) those e
 * doesn't have, for a profile saved before the rule had them, and RULE_ONLY's
 * where they differ. How many. */
int detect_apply_core_rules(cJSON *e, const char *system, const char *corename, int missing_only) {
    int added = 0;
    static cJSON *rules;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!rules) rules = cJSON_Parse(RETROARCH_CORES_JSON);
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(rules, "rules")) {
        const cJSON *names = cJSON_GetObjectItemCaseSensitive(r, "names"), *has = cJSON_GetObjectItemCaseSensitive(r, "contains"),
                    *systems = cJSON_GetObjectItemCaseSensitive(r, "systems");
        if ((names || has) && !name_in(names, corename, 0) && !name_in(has, corename, 1)) continue;
        if (systems && !name_in(systems, system, 0)) continue;
        const cJSON *k;
        cJSON_ArrayForEach(k, cJSON_GetObjectItemCaseSensitive(r, "entry")) {
            const cJSON *have = cJSON_GetObjectItemCaseSensitive(e, k->string);
            int rule_only = 0;
            for (size_t i = 0; i < sizeof RULE_ONLY / sizeof RULE_ONLY[0]; i++) rule_only |= !strcmp(k->string, RULE_ONLY[i]);
            if (missing_only && have && (!rule_only || cJSON_Compare(have, k, 1))) continue;
            cJSON_DeleteItemFromObjectCaseSensitive(e, k->string);
            cJSON_AddItemToObject(e, k->string, cJSON_Duplicate(k, 1));
            added++;
        }
        break;
    }
    pthread_mutex_unlock(&lock);
    return added;
}

/* One platform entry per system; the first core found for a system wins, and
 * the others are listed in its "cores": RetroArch picks one as a game starts,
 * so the saves of each that keeps them in a known format sync too (profiles.c
 * makes them twins). */
static void add_core_platforms(cJSON *plats, cJSON *cores, const char *core_id, const char *info) {
    char corename[64] = "", dbs[1024] = "";
    info_get(info, "corename", corename, sizeof corename);
    info_get(info, "database", dbs, sizeof dbs);
    if (!corename[0]) str_copy(corename, sizeof corename, core_id);
    cJSON_AddItemToArray(cores, cJSON_CreateString(corename));

    char *save = NULL;
    for (char *db = strtok_r(dbs, "|", &save); db; db = strtok_r(NULL, "|", &save)) {
        const char *slugs = NULL;
        for (size_t i = 0; i < sizeof DB_MAP / sizeof DB_MAP[0]; i++)
            if (!strcmp(DB_MAP[i].db, db)) slugs = DB_MAP[i].slugs;
        if (!slugs) continue;
        char first[64];
        str_copy(first, sizeof first, slugs);
        char *comma = strchr(first, ',');
        if (comma) *comma = 0;
        cJSON *p, *dup = NULL;
        cJSON_ArrayForEach(p, plats) if (!strcmp(jget_str(p, "dir", ""), first)) dup = p;
        if (dup) {
            /* (once a core: its databases can name one system twice) */
            if (!strcmp(jget_str(dup, "core_name", ""), corename)) continue;
            cJSON *also = cJSON_GetObjectItemCaseSensitive(dup, "cores"), *c0;
            int listed = 0;
            cJSON_ArrayForEach(c0, also) listed |= !strcmp(jget_str(c0, "core_name", ""), corename);
            if (listed) continue;
            if (!also) also = cJSON_AddArrayToObject(dup, "cores");
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "core_name", corename);
            cJSON_AddStringToObject(c, "emulator", core_id);
            cJSON_AddItemToArray(also, c);
            continue;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddItemToObject(e, "romm", slug_array(slugs));
        cJSON_AddStringToObject(e, "dir", first);
        cJSON_AddStringToObject(e, "db", db);
        cJSON_AddStringToObject(e, "emulator", core_id);
        cJSON_AddStringToObject(e, "core_name", corename);
        /* Saves other emulators keep in the same format (save_format) share
         * one save on RomM; save files, cards and layouts per core. */
        detect_apply_core_rules(e, first, corename, 0);
        cJSON_AddItemToArray(plats, e);
    }
}

/* Title and title id from a native app's sce_sys/param.json. */
static void app_title(const char *root, char *name, size_t n, char *title_id, size_t tn) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, root, "sce_sys/param.json");
    char *txt = read_file(p, NULL);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(j, "localizedParameters");
    const char *lang = jget_str(loc, "defaultLanguage", "en-US");
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(loc, lang);
    if (jget_str(l, "titleName", NULL)) str_copy(name, n, jget_str(l, "titleName", ""));
    if (jget_str(j, "titleId", NULL)) str_copy(title_id, tn, jget_str(j, "titleId", ""));
    cJSON_Delete(j);
}

/* A folder setting from retroarch.cfg, with ":", "~" and "/app0" meaning root.
 * Leaves out untouched if the key is unset or "default". */
static void cfg_dir(const char *cfg, const char *key, const char *root, char *out, size_t n) {
    char v[PATH_MAX_LEN];
    if (!cfg || !info_get(cfg, key, v, sizeof v) || !v[0] || !strcmp(v, "default")) return;
    if (v[0] == ':' || v[0] == '~') snprintf(out, n, "%s%s", root, v + 1);
    else if (!strncmp(v, "/app0", 5) && (v[5] == '/' || !v[5])) snprintf(out, n, "%s%s", root, v + 5);
    else str_copy(out, n, v);
}

static cJSON *retroarch_candidate(const char *root) {
    char p[PATH_MAX_LEN], cores_dir[PATH_MAX_LEN], info_dir[PATH_MAX_LEN];
    char *cfg = NULL;
    path_join(p, sizeof p, root, "config/retroarch.cfg");
    if (!(cfg = read_file(p, NULL))) {
        path_join(p, sizeof p, root, "retroarch.cfg");
        cfg = read_file(p, NULL);
    }
    int has_cfg = cfg != NULL;
    path_join(cores_dir, sizeof cores_dir, root, "cores");
    cfg_dir(cfg, "libretro_directory", root, cores_dir, sizeof cores_dir);
    str_copy(info_dir, sizeof info_dir, cores_dir);
    cfg_dir(cfg, "libretro_info_path", root, info_dir, sizeof info_dir);
    free(cfg);
    if (!has_cfg && !dir_exists(cores_dir)) return NULL;

    char name[128] = "RetroArch", title_id[16] = "";
    app_title(root, name, sizeof name, title_id, sizeof title_id);
    /* No app folder to name it (the PS4's builds): told apart by their folder. */
    if (!title_id[0] && strcmp(path_basename(root), "retroarch") != 0)
        snprintf(name, sizeof name, "RetroArch (%s)", path_basename(root));
    if (!strstr(name, "RetroArch") && !strstr(name, "retroarch") && !has_cfg) return NULL;

    cJSON *plats = cJSON_CreateArray(), *cores = cJSON_CreateArray();
    /* Core ids from the cores folder: snes9x from snes9x_libretro.info,
     * snes9x_libretro.prx or snes9x_libretro_ps4.self. */
    DIR *d = opendir(cores_dir);
    struct dirent *de;
    char names[128][128];
    int n = 0;
    while (d && (de = readdir(d)) && n < 128) {
        const char *tag = strstr(de->d_name, "_libretro");
        if (!tag || tag == de->d_name) continue;
        char id[128];
        snprintf(id, sizeof id, "%.*s", (int)(tag - de->d_name), de->d_name);
        int dup = 0;
        for (int i = 0; i < n && !dup; i++) dup = !strcmp(names[i], id);
        if (!dup) str_copy(names[n++], sizeof names[0], id);
    }
    if (d) closedir(d);
    /* RetroArch 2.0's PS5SX2 replaces LRPS2, whose files an upgrade can leave behind. */
    int has_ps5sx2 = 0;
    for (int i = 0; i < n; i++) has_ps5sx2 |= !strcmp(names[i], "ps5sx2");
    if (has_ps5sx2 && ra2_is_v2(root))
        for (int i = 0; i < n; i++)
            if (!strcmp(names[i], "pcsx2")) {
                LOGI("ignoring LRPS2 at %s: RetroArch 2.0 plays PS2 with PS5SX2", root);
                memmove(names[i], names[n - 1], sizeof names[0]);
                n--;
                break;
            }
    /* Sorted so the first core per system doesn't depend on readdir order. */
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++)
            if (strcmp(names[k], names[i]) < 0) {
                char t[128];
                memcpy(t, names[i], sizeof t);
                memcpy(names[i], names[k], sizeof t);
                memcpy(names[k], t, sizeof t);
            }
    LOGI("retroarch at %s: config %s, cores in %s (%d found), core info in %s", root, has_cfg ? "found" : "missing",
         cores_dir, n, info_dir);
    for (int i = 0; i < n; i++) {
        char info_name[160], info_path[PATH_MAX_LEN];
        snprintf(info_name, sizeof info_name, "%s_libretro.info", names[i]);
        path_join(info_path, sizeof info_path, info_dir, info_name);
        char *txt = read_file(info_path, NULL);
        if (!txt) {
            path_join(info_path, sizeof info_path, cores_dir, info_name);
            txt = read_file(info_path, NULL);
        }
        if (txt) add_core_platforms(plats, cores, names[i], txt);
        else LOGD("no %s for core %s", info_name, names[i]);
        free(txt);
    }

    /* Folder layout comes from the built-in RetroArch profile. */
    cJSON *defs = profiles_default(), *profile = NULL, *pr;
    cJSON_ArrayForEach(pr, defs) if (!strcmp(jget_str(pr, "id", ""), "retroarch")) profile = pr;
    profile = cJSON_Duplicate(profile, 1);
    cJSON_Delete(defs);
    jset_str(profile, "root", root);
    char id[48];
    snprintf(id, sizeof id, "retroarch-%s", title_id[0] ? title_id : path_basename(root));
    jset_str(profile, "id", id);
    jset_str(profile, "name", name);
    jset_num(profile, "generated", PROFILE_GENERATION);
    if (cJSON_GetArraySize(plats) > 0) {
        cJSON_ReplaceItemInObjectCaseSensitive(profile, "platforms", plats);
    } else {
        cJSON_Delete(plats); /* no .info files, keep the default platforms */
    }
    ra2_update_profile(profile, NULL);

    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "kind", "retroarch");
    cJSON_AddStringToObject(c, "name", name);
    cJSON_AddStringToObject(c, "root", root);
    cJSON_AddStringToObject(c, "title_id", title_id);
    cJSON_AddItemToObject(c, "cores", cores);
    cJSON_AddItemToObject(c, "profile", profile);
    return c;
}

static cJSON *mednafen_candidate(const char *root) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, root, "mednafen.cfg");
    if (!file_exists(p)) return NULL;
    cJSON *defs = profiles_default(), *profile = NULL, *pr;
    cJSON_ArrayForEach(pr, defs) if (!strcmp(jget_str(pr, "id", ""), "mednafen")) profile = pr;
    profile = cJSON_Duplicate(profile, 1);
    cJSON_Delete(defs);
    jset_str(profile, "root", root);
    cJSON_ReplaceItemInObjectCaseSensitive(profile, "enabled", cJSON_CreateTrue());
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "kind", "mednafen");
    cJSON_AddStringToObject(c, "name", "Mednafen");
    cJSON_AddStringToObject(c, "root", root);
    cJSON_AddStringToObject(c, "title_id", "");
    cJSON_AddItemToObject(c, "cores", cJSON_CreateArray());
    cJSON_AddItemToObject(c, "profile", profile);
    return c;
}

/* Installed apps live in /user/app/<title id>, or the same on extended storage.
 * ROMM_SYNC_APP_DIRS (comma separated) replaces the list, for tests. */
static int app_installed(const char *title_id) {
    const char *env = getenv("ROMM_SYNC_APP_DIRS");
    char dirs[PATH_MAX_LEN], p[PATH_MAX_LEN], *save = NULL;
    str_copy(dirs, sizeof dirs, env && *env ? env : "/user/app,/mnt/ext0/user/app,/mnt/ext1/user/app");
    for (char *d = strtok_r(dirs, ",", &save); d; d = strtok_r(NULL, ",", &save)) {
        path_join(p, sizeof p, d, title_id);
        if (dir_exists(p)) return 1;
    }
    return 0;
}

/* Presets typically are {"format", "version", "console" "emulators": [....]}*/
static cJSON *catalog_from(cJSON *file) {
    if (cJSON_IsArray(file)) return file;
    cJSON *list = cJSON_DetachItemFromObjectCaseSensitive(file, "emulators");
    cJSON_Delete(file);
    return list;
}

/* The catalog: ROMM_SYNC_CATALOG names a file (for tests), else the console's. */
static cJSON *emulator_catalog(void) {
    const char *env = getenv("ROMM_SYNC_CATALOG");
    if (env && *env) {
        char *txt = read_file(env, NULL);
        cJSON *j = txt ? cJSON_Parse(txt) : NULL;
        free(txt);
        return catalog_from(j);
    }
    return cJSON_Duplicate(presets_catalog(), 1);
}

static cJSON *g_catalog;
static void load_catalog(void) { g_catalog = emulator_catalog(); }

const cJSON *detect_catalog_entry(const char *id) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    const char *env = getenv("ROMM_SYNC_CATALOG");
    const cJSON *catalog = presets_catalog(), *e;
    if (env && *env) {
        pthread_once(&once, load_catalog);
        catalog = g_catalog;
    }
    cJSON_ArrayForEach(e, catalog)
        if (!strcmp(jget_str(e, "id", ""), id)) return e;
    return NULL;
}

static void add_standalone(cJSON *out) {
    cJSON *catalog = emulator_catalog();
    const cJSON *e, *x;
    cJSON_ArrayForEach(e, catalog) {
        char found[16] = "";
        int present = 0;
        cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(e, "title_ids"))
            if (!present && cJSON_IsString(x) && app_installed(x->valuestring)) {
                str_copy(found, sizeof found, x->valuestring);
                present = 1;
            }
        cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(e, "detect"))
            if (!present && cJSON_IsString(x) && dir_exists(x->valuestring)) present = 1;
        if (!present) continue;
        /* Found, but with no preset yet: setup offers it with folders the user
         * picks (a custom profile), not this profile. */
        const int ready = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(e, "ready"));
        cJSON *profile = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(e, "profile"), 1);
        if (!profile) continue;
        jset_str(profile, "id", jget_str(e, "id", "standalone"));
        jset_str(profile, "name", jget_str(e, "name", "Emulator"));
        cJSON_DeleteItemFromObjectCaseSensitive(profile, "enabled");
        cJSON_AddBoolToObject(profile, "enabled", 1);
        jset_num(profile, "generated", PROFILE_GENERATION);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "kind", "standalone");
        cJSON_AddStringToObject(c, "id", jget_str(e, "id", "standalone"));
        cJSON_AddStringToObject(c, "name", jget_str(e, "name", ""));
        cJSON_AddStringToObject(c, "root", jget_str(profile, "root", ""));
        cJSON_AddStringToObject(c, "title_id", found);
        cJSON_AddItemToObject(c, "cores", cJSON_CreateArray());
        cJSON_AddItemToObject(c, "profile", profile);
        cJSON_AddBoolToObject(c, "ready", ready);
        cJSON_AddStringToObject(c, "note", jget_str(e, "note", ""));
        LOGI("detected %s (%s)%s", jget_str(e, "name", ""), found[0] ? found : jget_str(c, "root", ""),
             ready ? "" : ", no preset yet");
        cJSON_AddItemToArray(out, c);
    }
    cJSON_Delete(catalog);
}

static void try_root(cJSON *out, const char *root) {
    if (!dir_exists(root)) return;
    cJSON *c = retroarch_candidate(root);
    if (!c) c = mednafen_candidate(root);
    if (c) {
        LOGI("detected %s at %s", jget_str(c, "kind", ""), root);
        cJSON_AddItemToArray(out, c);
    }
}

cJSON *detect_emulators(void) {
    cJSON *out = cJSON_CreateArray();
    const plat_info_t *pi = plat_info();
    const char *env = getenv("ROMM_SYNC_HOMEBREW"); /* for tests */
    const char *bases[17];
    int nb = 0;
    if (env && *env) bases[nb++] = env;
    for (int i = 0; pi->homebrew_dirs[i] && nb < 16; i++) bases[nb++] = pi->homebrew_dirs[i];
    bases[nb] = NULL;

    for (int i = 0; pi->emulator_dirs[i]; i++) try_root(out, pi->emulator_dirs[i]);
    for (int b = 0; bases[b]; b++) {
        DIR *d = opendir(bases[b]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char root[PATH_MAX_LEN];
            path_join(root, sizeof root, bases[b], de->d_name);
            try_root(out, root);
        }
        closedir(d);
    }
    add_standalone(out);
    /* The folders each emulator uses by default, for a given platform. */
    cJSON *c;
    cJSON_ArrayForEach(c, out) {
        if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(c, "ready"))) continue; /* No preset, custom */
        cJSON *profile = cJSON_GetObjectItemCaseSensitive(c, "profile"), *one = cJSON_CreateArray();
        cJSON_AddItemReferenceToArray(one, profile);
        profile_map *maps = NULL;
        int n = profiles_expand(one, &maps);
        cJSON *dirs = cJSON_AddObjectToObject(c, "dirs");
        for (int i = 0; i < n; i++) {
            char key[64];
            str_copy(key, sizeof key, maps[i].romm_slugs);
            char *comma = strchr(key, ',');
            if (comma) *comma = 0;
            cJSON *d = cJSON_AddObjectToObject(dirs, key);
            cJSON_AddStringToObject(d, "rom_dir", maps[i].rom_dir);
            cJSON_AddStringToObject(d, "save_dir", maps[i].save_dir);
            cJSON_AddStringToObject(d, "state_dir", maps[i].state_dir);
            /* Games already there, so setup can pick the emulator they're in. */
            int games = 0;
            DIR *g = opendir(maps[i].rom_dir);
            struct dirent *ge;
            while (g && (ge = readdir(g)))
                if (ge->d_name[0] != '.' && !ext_in_list(ge->d_name, maps[i].save_exts) &&
                    !ext_in_list(ge->d_name, maps[i].state_exts) && !ext_in_list(ge->d_name, ".part,.tmp,.txt,.png,.jpg,.xml"))
                    games++;
            if (g) closedir(g);
            cJSON_AddNumberToObject(d, "games", games);
        }
        profiles_free(maps);
        cJSON_Delete(one);
        const cJSON *fixed = cJSON_GetObjectItemCaseSensitive(profile, "fixed");
        cJSON_AddItemToObject(c, "fixed", fixed ? cJSON_Duplicate(fixed, 1) : cJSON_CreateArray());
    }
    return out;
}

/* Keys a newer preset has that a saved profile lacks (a state folder added
 * to Porpoise's, say) are added to it. Nothing already there changes, so
 * folders the user picked stay; but for the keys nobody can set but the
 * preset, which follow it (PSXS5's cores, as its versions change them). */
static const char *const PRESET_ONLY[] = {"state_core_setting"};

/* Folders and names a preset used to have and has changed since: a saved
 * copy of one is the old preset's, not the user's pick, so it follows the
 * preset now. When a preset's value changes, its old one goes here, or the
 * profiles saved with it keep it. */
static const struct {
    const char *profile, *key, *value;
} SHIPPED_VALUES[] = {
    {"seedee5", "save_dir", "{root}/saves"}, /* CD-i saves: each game's NVRAM, 1.4.0 */
    {"ps5sx2", "bios_dir", "{root}"},        /* PS5SX2 2.x lists its BIOS in bios/, 1.4.0 */
    {"xpsemu", "rom_dir", "{root}/games"},   /* XPSemu 3's games go in /data/xpsemu/games (old_rom_dirs keeps Alpha 2's) */
    /* SwanStationPS5 names cards and states by serial once it knows it, 1.4.0 */
    {"swanstationps5", "save_name", "{rom_stem}_1{ext}"},
    {"swanstationps5", "save_stems", "{rom_stem}_1"},
    {"swanstationps5", "state_stems", "{rom_stem_safe}"},
    {"swanstationps5", "state_name", "{rom_stem_safe}{ext}"},
    {"ps5cemu", "name", "PS5CEMU-HAR"}, /* renamed PS5-ECAM by its developer, 1.4.0 */
    /* PS5-ECAM 5.1 moved each system out of /data/ps5cemu into its own folder, 1.4.0 */
    {"ps5cemu", "rom_dir", "{root}/games"},
    {"ps5cemu", "rom_dir", "{root}/azahar/games"},
    {"ps5cemu", "rom_dir", "{root}/melonds/games"},
    {"ps5cemu", "rom_dir", "{root}/eden/roms"},
    {"ps5cemu", "bios_dir", "{root}"},
    {"ps5cemu", "bios_dir", "{root}/azahar/sysdata"},
    {"ps5cemu", "bios_dir", "{root}/melonds/bios"},
    {"ps5cemu", "bios_dir", "{root}/eden"},
    {"ps5cemu", "save_dir", "{root}/melonds/saves"},
    {"ps5cemu", "state_dir", "{root}/azahar/states"},
    {"ps5cemu", "state_dir", "{root}/melonds/states"},
    {"ps5cemu", "bundle_dirs", "{root}/mlc01/usr/save"},
    {"ps5cemu", "bundle_dirs", "{root}/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title"},
    {"ps5cemu", "bundle_dirs", "{root}/eden/user/nand/user/save/0000000000000000/{switch_user}"},
    {"ps5cemu", "switch_users", "{root}/eden/config/prosperoeden.json"},
    {"mupen64ps5", "save_dir", "{root}/saves"}, /* its own files now go through a .srm of RommPS's, 1.4.0 */
    /* Test builds before it was the user's choice (PS5-ECAM asks): new folder first */
    {"ps5cemu", "user_dir", "/data/cemu,{root}"},
    {"ps5cemu", "user_dir", "/data/azahar,{root}/azahar"},
    {"ps5cemu", "user_dir", "/data/melonds,{root}/melonds"},
    {"ps5cemu", "user_dir", "/data/eden,{root}/eden"},
    {"ps5cemu", "user_dir_has", "mlc01"},
};

static int shipped_value(const char *profile, const char *key, const cJSON *have) {
    for (size_t i = 0; i < sizeof SHIPPED_VALUES / sizeof SHIPPED_VALUES[0]; i++)
        if (!strcmp(SHIPPED_VALUES[i].profile, profile) && !strcmp(SHIPPED_VALUES[i].key, key) &&
            cJSON_IsString(have) && !strcmp(have->valuestring, SHIPPED_VALUES[i].value))
            return 1;
    return 0;
}

/* "sync_saves" says whether an emulator's saves can sync, and only the preset
 * gets to say it now (profiles.c). Saved profiles still carry copies from
 * before: a "false" the preset never shipped was the user switching sync off,
 * which "sync": "off" says now; one it did ship (and has since dropped, the
 * saves turning out fine) is simply gone. Every "false" ever shipped, by
 * emulator and system ("*": all of them): */
static const struct {
    const char *profile, *system;
} SHIPPED_NO_SYNC[] = {
    {"ps5cemu", "nds"}, {"prosperoeden", "*"}, {"ps4sx2", "*"}, {"ps5sx2", "*"},
    {"resonance", "*"}, {"seedee5", "*"},      {"xpsemu", "*"},
    /* new in 1.4.0 with games only, then saves too */
    {"beetlepce", "*"}, {"genplusgx", "*"}, {"snes9xps5", "*"}, {"fbneops5", "*"}, {"psp5", "*"}, {"mupen64ps5", "*"},
};

static int shipped_no_sync(const char *profile, const char *system) {
    for (size_t i = 0; i < sizeof SHIPPED_NO_SYNC / sizeof SHIPPED_NO_SYNC[0]; i++)
        if (!strcmp(SHIPPED_NO_SYNC[i].profile, profile) &&
            (!strcmp(SHIPPED_NO_SYNC[i].system, "*") || !strcmp(SHIPPED_NO_SYNC[i].system, system)))
            return 1;
    return 0;
}

/* One level's saved copy (the profile's, system "*", or a platform's) gone;
 * the user's own "off" kept as "sync". `preset` is what the preset says for
 * that level now: a copy of it isn't anyone's choice either. */
static int drop_saved_sync_saves(cJSON *o, const cJSON *preset, const char *profile, const char *system, const char *name) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "sync_saves");
    if (!v) return 0;
    if (cJSON_IsFalse(v) && !cJSON_IsFalse(preset) && !shipped_no_sync(profile, system)) {
        cJSON_DeleteItemFromObjectCaseSensitive(o, "sync");
        cJSON_AddStringToObject(o, "sync", "off");
        LOGI("the %s profile: sync turned off for %s, as its saved profile had it", name, !strcmp(system, "*") ? "all its systems" : system);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(o, "sync_saves");
    return 1;
}

static int drop_saved_sync(cJSON *p, const cJSON *preset) {
    const char *id = jget_str(p, "id", ""), *name = jget_str(p, "name", "");
    const cJSON *top = cJSON_GetObjectItemCaseSensitive(preset, "sync_saves");
    int n = drop_saved_sync_saves(p, top, id, "*", name);
    cJSON *pe;
    cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
        const cJSON *ce, *says = top;
        cJSON_ArrayForEach(ce, cJSON_GetObjectItemCaseSensitive(preset, "platforms"))
            if (!strcmp(profiles_system_key(ce), profiles_system_key(pe)) && cJSON_GetObjectItemCaseSensitive(ce, "sync_saves"))
                says = cJSON_GetObjectItemCaseSensitive(ce, "sync_saves");
        n += drop_saved_sync_saves(pe, says, id, profiles_system_key(pe), name);
    }
    return n;
}

static int fill_from_catalog(cJSON *p) {
    const cJSON *entry = detect_catalog_entry(jget_str(p, "id", ""));
    const cJSON *preset = entry ? cJSON_GetObjectItemCaseSensitive(entry, "profile") : NULL, *k;
    cJSON *pe;
    int added = 0;
    /* A RetroArch core's platform entry saved before its rule had a key
     * (memcards, cards...): what the rule says now. */
    cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(pe, "romm"), 0);
        const char *core = jget_str(pe, "core_name", "");
        if (cJSON_IsString(first) && core[0]) added += detect_apply_core_rules(pe, first->valuestring, core, 1);
    }
    if (!preset) {
        if (added) LOGI("the %s profile got %d setting%s from RetroArch's core rules", jget_str(p, "name", ""), added, added == 1 ? "" : "s");
        return added;
    }
    cJSON_ArrayForEach(k, preset) {
        if (!strcmp(k->string, "platforms") || !strcmp(k->string, "sync_saves")) continue; /* (the preset's alone) */
        const cJSON *have = cJSON_GetObjectItemCaseSensitive(p, k->string);
        int preset_only = 0;
        for (size_t i = 0; i < sizeof PRESET_ONLY / sizeof PRESET_ONLY[0]; i++) preset_only |= !strcmp(k->string, PRESET_ONLY[i]);
        if (!have) {
            cJSON_AddItemToObject(p, k->string, cJSON_Duplicate(k, 1));
            added++;
        } else if ((preset_only || shipped_value(jget_str(p, "id", ""), k->string, have)) && !cJSON_Compare(have, k, 1)) {
            cJSON_ReplaceItemInObjectCaseSensitive(p, k->string, cJSON_Duplicate(k, 1));
            added++;
        }
    }
    /* An emulator renamed by its developer: a saved name the preset shipped
     * takes the new one (the name sits beside the profile, not in it). */
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name"), *had = cJSON_GetObjectItemCaseSensitive(p, "name");
    if (cJSON_IsString(name) && had && shipped_value(jget_str(p, "id", ""), "name", had) && !cJSON_Compare(had, name, 1)) {
        LOGI("%s is called %s now", had->valuestring, name->valuestring);
        cJSON_ReplaceItemInObjectCaseSensitive(p, "name", cJSON_Duplicate(name, 1));
        added++;
    }
    const int dropped = drop_saved_sync(p, preset);
    cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(pe, "romm"), 0), *ce;
        if (!cJSON_IsString(first)) continue;
        cJSON_ArrayForEach(ce, cJSON_GetObjectItemCaseSensitive(preset, "platforms")) {
            const cJSON *cf = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(ce, "romm"), 0);
            if (!cJSON_IsString(cf) || strcmp(cf->valuestring, first->valuestring)) continue;
            cJSON_ArrayForEach(k, ce) {
                if (!strcmp(k->string, "sync_saves")) continue;
                const cJSON *have = cJSON_GetObjectItemCaseSensitive(pe, k->string);
                if (!have) {
                    cJSON_AddItemToObject(pe, k->string, cJSON_Duplicate(k, 1));
                    added++;
                } else if (shipped_value(jget_str(p, "id", ""), k->string, have) && !cJSON_Compare(have, k, 1)) {
                    cJSON_ReplaceItemInObjectCaseSensitive(pe, k->string, cJSON_Duplicate(k, 1));
                    added++;
                }
            }
        }
    }
    if (added) LOGI("the %s profile got %d setting%s from its newer preset", jget_str(p, "name", ""), added, added == 1 ? "" : "s");
    return added + dropped;
}

/* A system a newer preset adds to an emulator (Switch to PS5-ECAM, for
 * Eden) joins its saved profile. When another emulator already plays that
 * system, the new one comes in excluded from it, as the user's pick stands:
 * Emulator Settings can hand it over. */
static int add_new_platforms(cJSON *profiles) {
    int added = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, profiles) {
        const cJSON *entry = detect_catalog_entry(jget_str(p, "id", ""));
        const cJSON *preset = entry ? cJSON_GetObjectItemCaseSensitive(entry, "profile") : NULL, *ce, *pe;
        cJSON *mine = cJSON_GetObjectItemCaseSensitive(p, "platforms");
        if (!preset || !cJSON_IsArray(mine)) continue;
        cJSON_ArrayForEach(ce, cJSON_GetObjectItemCaseSensitive(preset, "platforms")) {
            const char *key = profiles_system_key(ce);
            int have = !key[0];
            cJSON_ArrayForEach(pe, mine) have |= !strcmp(profiles_system_key(pe), key);
            if (have) continue;
            cJSON_AddItemToArray(mine, cJSON_Duplicate(ce, 1));
            const cJSON *o;
            int taken = 0;
            cJSON_ArrayForEach(o, profiles) {
                if (o == p || cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "enabled")) || profiles_excludes(o, key)) continue;
                cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(o, "platforms")) taken |= !strcmp(profiles_system_key(pe), key);
            }
            if (taken) {
                cJSON *ex = cJSON_GetObjectItemCaseSensitive(p, "exclude");
                if (!cJSON_IsArray(ex)) ex = cJSON_AddArrayToObject(p, "exclude");
                cJSON_AddItemToArray(ex, cJSON_CreateString(key));
            }
            LOGI("the %s profile plays %s now%s", jget_str(p, "name", ""), key, taken ? " (another emulator keeps it)" : "");
            added++;
        }
    }
    return added;
}

int detect_refresh_profiles(cJSON *profiles) {
    int changed = 0;
    cJSON *found = NULL;
    cJSON *each;
    cJSON_ArrayForEach(each, profiles) changed += fill_from_catalog(each) > 0;
    changed += add_new_platforms(profiles);
    for (int i = 0; i < cJSON_GetArraySize(profiles); i++) {
        cJSON *p = cJSON_GetArrayItem(profiles, i);
        const char *id = jget_str(p, "id", "");
        int gen = (int)jget_num(p, "generated", 0);
        /* Generated by setup: current ones carry "generated", older ones only the id. */
        if (gen >= PROFILE_GENERATION || (gen == 0 && strncmp(id, "retroarch-", 10) != 0)) continue;
        if (!found) found = detect_emulators();
        const cJSON *c;
        cJSON_ArrayForEach(c, found) {
            if (strcmp(jget_str(c, "root", ""), jget_str(p, "root", "")) != 0) continue;
            cJSON *fresh = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(c, "profile"), 1);
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(p, "enabled");
            if (cJSON_IsBool(en)) cJSON_ReplaceItemInObjectCaseSensitive(fresh, "enabled", cJSON_CreateBool(cJSON_IsTrue(en)));
            /* The user's choice of emulator per system survives a refresh. */
            const cJSON *ex = cJSON_GetObjectItemCaseSensitive(p, "exclude");
            if (ex) cJSON_AddItemToObject(fresh, "exclude", cJSON_Duplicate(ex, 1));
            const cJSON *extra = cJSON_GetObjectItemCaseSensitive(p, "extra");
            if (extra) cJSON_AddItemToObject(fresh, "extra", cJSON_Duplicate(extra, 1));
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(p, "sync");
            if (cJSON_IsString(mode)) cJSON_AddStringToObject(fresh, "sync", mode->valuestring);
            /* ... and each platform's. */
            cJSON *fe;
            cJSON_ArrayForEach(fe, cJSON_GetObjectItemCaseSensitive(fresh, "platforms")) {
                const cJSON *pe, *m;
                cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(p, "platforms"))
                    if (!strcmp(profiles_system_key(pe), profiles_system_key(fe)) &&
                        cJSON_IsString(m = cJSON_GetObjectItemCaseSensitive(pe, "sync")))
                        cJSON_AddStringToObject(fe, "sync", m->valuestring);
            }
            LOGI("updated the %s profile for %s", jget_str(fresh, "name", ""), jget_str(p, "root", ""));
            cJSON_ReplaceItemInArray(profiles, i, fresh);
            changed++;
            break;
        }
    }
    /* A RetroArch's other cores per system, as they are now: installed or
     * removed since, or a profile saved before they were listed. */
    cJSON_ArrayForEach(each, profiles) {
        if (strncmp(jget_str(each, "id", ""), "retroarch-", 10) != 0) continue;
        if (!found) found = detect_emulators();
        const cJSON *c;
        cJSON_ArrayForEach(c, found) {
            if (strcmp(jget_str(c, "root", ""), jget_str(each, "root", "")) != 0) continue;
            const cJSON *fresh = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c, "profile"), "platforms"), *fe;
            cJSON *pe;
            cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(each, "platforms")) {
                const cJSON *now = NULL, *had = cJSON_GetObjectItemCaseSensitive(pe, "cores");
                cJSON_ArrayForEach(fe, fresh)
                    if (!strcmp(profiles_system_key(fe), profiles_system_key(pe))) now = cJSON_GetObjectItemCaseSensitive(fe, "cores");
                if ((!now && !had) || (now && had && cJSON_Compare(now, had, 1))) continue;
                cJSON_DeleteItemFromObjectCaseSensitive(pe, "cores");
                if (now) cJSON_AddItemToObject(pe, "cores", cJSON_Duplicate(now, 1));
                changed++;
            }
        }
    }
    cJSON_Delete(found);
    return changed;
}
