/* Finds emulators under homebrew/ on internal storage and USB drives. For
 * RetroArch the platform list comes from the cores folder. Each .info file gives
 * "corename", the folder name for sorted saves, and "database", its systems. */
#include "detect.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static cJSON *retroarch_candidate(const char *root) {
    char p[PATH_MAX_LEN], cores_dir[PATH_MAX_LEN];
    path_join(cores_dir, sizeof cores_dir, root, "cores");
    int has_cfg = 0;
    path_join(p, sizeof p, root, "config/retroarch.cfg");
    has_cfg |= file_exists(p);
    path_join(p, sizeof p, root, "retroarch.cfg");
    has_cfg |= file_exists(p);
    if (!has_cfg && !dir_exists(cores_dir)) return NULL;

    char name[128] = "RetroArch", title_id[16] = "";
    app_title(root, name, sizeof name, title_id, sizeof title_id);
    if (!strstr(name, "RetroArch") && !strstr(name, "retroarch") && !has_cfg) return NULL;

    cJSON *plats = cJSON_CreateArray(), *cores = cJSON_CreateArray();
    DIR *d = opendir(cores_dir);
    struct dirent *de;
    char names[64][128];
    int n = 0;
    while (d && (de = readdir(d)) && n < 64)
        if (str_ends_with_ci(de->d_name, "_libretro.info")) str_copy(names[n++], sizeof names[0], de->d_name);
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
    for (int i = 0; i < n; i++) {
        char info_path[PATH_MAX_LEN], core_id[128];
        path_join(info_path, sizeof info_path, cores_dir, names[i]);
        str_copy(core_id, sizeof core_id, names[i]);
        core_id[strlen(core_id) - strlen("_libretro.info")] = 0;
        char *txt = read_file(info_path, NULL);
        if (txt) add_core_platforms(plats, cores, core_id, txt);
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

cJSON *detect_emulators(void) {
    cJSON *out = cJSON_CreateArray();
    char bases[16][PATH_MAX_LEN];
    int nb = 0;
    const char *env = getenv("ROMM_SYNC_HOMEBREW"); /* for tests */
    if (env && *env) str_copy(bases[nb++], sizeof bases[0], env);
    str_copy(bases[nb++], sizeof bases[0], "/data/homebrew");
    for (int i = 0; i < 4; i++) snprintf(bases[nb++], sizeof bases[0], "/mnt/usb%d/homebrew", i);
    for (int i = 0; i < 2; i++) snprintf(bases[nb++], sizeof bases[0], "/mnt/ext%d/homebrew", i);

    for (int b = 0; b < nb; b++) {
        DIR *d = opendir(bases[b]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char root[PATH_MAX_LEN];
            path_join(root, sizeof root, bases[b], de->d_name);
            if (!dir_exists(root)) continue;
            cJSON *c = retroarch_candidate(root);
            if (!c) c = mednafen_candidate(root);
            if (c) {
                LOGI("detected %s at %s", jget_str(c, "kind", ""), root);
                cJSON_AddItemToArray(out, c);
            }
        }
        closedir(d);
    }
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
            LOGI("updated the %s profile for %s", jget_str(fresh, "name", ""), jget_str(p, "root", ""));
            cJSON_ReplaceItemInArray(profiles, i, fresh);
            changed++;
            break;
        }
    }
    cJSON_Delete(found);
    return changed;
}
