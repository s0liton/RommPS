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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "profiles.h"
#include "state.h"
#include "util.h"

/* Bump this when generated profiles gain new keys, or existing installs keep
 * their old profile forever (ask me how I know). */
#define PROFILE_GENERATION 2

/* libretro database names to RomM slugs. The first slug is the folder name. */
static const struct {
    const char *db;
    const char *slugs;
} DB_MAP[] = {
    {"Nintendo - Nintendo Entertainment System", "nes,famicom"},
    {"Nintendo - Family Computer Disk System", "fds"},
    {"Nintendo - Super Nintendo Entertainment System", "snes,sfam"},
    {"Nintendo - Satellaview", "satellaview"},
    {"Nintendo - Game Boy", "gb"},
    {"Nintendo - Game Boy Color", "gbc"},
    {"Nintendo - Game Boy Advance", "gba"},
    {"Nintendo - Nintendo 64", "n64"},
    {"Nintendo - Nintendo DS", "nds"},
    {"Nintendo - Virtual Boy", "virtualboy"},
    {"Nintendo - GameCube", "ngc,gamecube"},
    {"Nintendo - Wii", "wii"},
    {"Sega - Mega Drive - Genesis", "genesis,genesis-slash-megadrive,megadrive"},
    {"Sega - Master System - Mark III", "sms,mastersystem"},
    {"Sega - Game Gear", "gamegear,gg"},
    {"Sega - Mega-CD - Sega CD", "segacd,sega-cd"},
    {"Sega - SG-1000", "sg1000"},
    {"Sega - 32X", "sega32,sega-32x"},
    {"Sega - Saturn", "saturn"},
    {"Sega - Dreamcast", "dc,dreamcast"},
    {"Sony - PlayStation", "psx,ps1"},
    {"Sony - PlayStation 2", "ps2"},
    {"Sony - PlayStation Portable", "psp"},
    {"FBNeo - Arcade Games", "arcade,fbneo,neogeoaes,neogeomvs,cps1,cps2,cps3"},
    {"MAME", "arcade,mame"},
    {"NEC - PC Engine - TurboGrafx 16", "tg16,turbografx16--1,pce,pcengine"},
    {"NEC - PC Engine CD - TurboGrafx-CD", "turbografx-cd,pcecd"},
    {"SNK - Neo Geo Pocket", "neo-geo-pocket,ngp"},
    {"SNK - Neo Geo Pocket Color", "neo-geo-pocket-color,ngpc"},
    {"Bandai - WonderSwan", "wonderswan,ws"},
    {"Bandai - WonderSwan Color", "wonderswan-color,wsc"},
    {"Atari - 2600", "atari2600"},
    {"Atari - 7800", "atari7800"},
    {"Atari - Lynx", "lynx"},
    {"Commodore - 64", "c64"},
    {"Commodore - Amiga", "amiga"},
    {"DOS", "dos"},
};

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

/* The save format a RetroArch core keeps, where another emulator keeps the
 * same: a raw 128 KB PS1 memory card, or N64's combined .srm (EEPROM, four
 * controller paks, SRAM and flash). */
static const char *save_format_of(const char *system, const char *corename) {
    if (!strcmp(system, "psx") && (strstr(corename, "Beetle PSX") || str_ieq(corename, "PCSX-ReARMed") ||
                                   str_ieq(corename, "SwanStation")))
        return "psx-card";
    if (!strcmp(system, "n64") && (strstr(corename, "ParaLLEl") || strstr(corename, "Mupen64Plus")))
        return "n64-srm";
    return NULL;
}

/* One platform entry per system; the first core found for a system wins. */
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
        int dup = 0;
        const cJSON *p;
        cJSON_ArrayForEach(p, plats) if (!strcmp(jget_str(p, "dir", ""), first)) dup = 1;
        if (dup) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddItemToObject(e, "romm", slug_array(slugs));
        cJSON_AddStringToObject(e, "dir", first);
        cJSON_AddStringToObject(e, "emulator", core_id);
        cJSON_AddStringToObject(e, "core_name", corename);
        /* Saves other emulators keep in the same format share one save on RomM. */
        const char *format = save_format_of(first, corename);
        if (format) cJSON_AddStringToObject(e, "save_format", format);
        if (str_ieq(corename, "LRPS2") || str_ieq(corename, "PCSX2")) {
            /* Per-game cards (<game>.ps2) sync like any other save. */
            cJSON_AddStringToObject(e, "save_exts", ".ps2");
            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "key", "pcsx2_shared_memory_cards");
            cJSON_AddStringToObject(req, "value", "disabled");
            cJSON_AddStringToObject(req, "default", "enabled");
            cJSON_AddStringToObject(req, "hint", "LRPS2 uses one shared memory card for all games. Turn off Shared Memory Cards in the core options to give each game its own card, which then syncs.");
            cJSON_AddItemToObject(e, "sync_requires_option", req);
        } else if (str_ieq(corename, "PPSSPP")) {
            cJSON_AddStringToObject(e, "save_layout", "psp");
        } else if (str_ieq(corename, "Dolphin")) {
            cJSON_AddStringToObject(e, "save_layout", !strcmp(first, "wii") ? "wii" : "gci");
        }
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

/* The catalog: ROMM_SYNC_CATALOG names a file (for tests), else the console's. */
static cJSON *emulator_catalog(void) {
    const char *env = getenv("ROMM_SYNC_CATALOG");
    if (env && *env) {
        char *txt = read_file(env, NULL);
        cJSON *j = txt ? cJSON_Parse(txt) : NULL;
        free(txt);
        return j;
    }
    return plat_info()->emulator_catalog ? cJSON_Parse(plat_info()->emulator_catalog) : NULL;
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
        /* Found but not supported yet: shown in setup, never a profile. */
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
        cJSON_AddStringToObject(c, "name", jget_str(e, "name", ""));
        cJSON_AddStringToObject(c, "root", jget_str(profile, "root", ""));
        cJSON_AddStringToObject(c, "title_id", found);
        cJSON_AddItemToObject(c, "cores", cJSON_CreateArray());
        cJSON_AddItemToObject(c, "profile", profile);
        cJSON_AddBoolToObject(c, "ready", ready);
        cJSON_AddStringToObject(c, "note", jget_str(e, "note", ""));
        LOGI("detected %s (%s)%s", jget_str(e, "name", ""), found[0] ? found : jget_str(c, "root", ""),
             ready ? "" : ", not supported yet");
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
    return out;
}

int detect_refresh_profiles(cJSON *profiles) {
    int changed = 0;
    cJSON *found = NULL;
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
            LOGI("updated the %s profile for %s", jget_str(fresh, "name", ""), jget_str(p, "root", ""));
            cJSON_ReplaceItemInArray(profiles, i, fresh);
            changed++;
            break;
        }
    }
    cJSON_Delete(found);
    return changed;
}
