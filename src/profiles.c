#include "profiles.h"

#include <ctype.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "detect.h"
#include "library.h"
#include "platform.h"
#include "state.h"

/* Platforms are matched on RomM's slug or fs_slug. The roots are the PS5's;
 * profiles_default() swaps in the console's own (plat_info). */
static const char *DEFAULT_PROFILES_JSON =
    "["
    /* RetroArch as a native app. /app0 is its install folder. */
    "{\"id\":\"retroarch\",\"name\":\"RetroArch\",\"enabled\":true,"
    "\"root\":\"/data/homebrew/PPSA99169\","
    "\"retroarch_cfg\":\"{root}/config/retroarch.cfg,{root}/retroarch.cfg\","
    "\"rom_dir\":\"{root}/content/{platform}\","
    "\"save_dir\":\"{root}/savefiles\","
    "\"state_dir\":\"{root}/savestates\","
    "\"bios_dir\":\"{root}/system\","
    "\"save_exts\":\".srm,.sav,.rtc\","
    "\"state_exts\":\".state*\","
    "\"save_name\":\"{rom_stem}{ext}\","
    "\"sync_saves\":true,"
    "\"platforms\":["
    "{\"romm\":[\"nes\",\"famicom\",\"fds\"],\"dir\":\"nes\",\"emulator\":\"fceumm\",\"core_name\":\"FCEUmm\"},"
    "{\"romm\":[\"snes\",\"sfam\",\"satellaview\"],\"dir\":\"snes\",\"emulator\":\"snes9x\",\"core_name\":\"Snes9x\"},"
    "{\"romm\":[\"gba\"],\"dir\":\"gba\",\"emulator\":\"mgba\",\"core_name\":\"mGBA\"},"
    "{\"romm\":[\"gb\"],\"dir\":\"gb\",\"emulator\":\"mgba\",\"core_name\":\"mGBA\"},"
    "{\"romm\":[\"gbc\"],\"dir\":\"gbc\",\"emulator\":\"mgba\",\"core_name\":\"mGBA\"},"
    "{\"romm\":[\"genesis\",\"genesis-slash-megadrive\",\"megadrive\"],\"dir\":\"megadrive\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"sms\",\"mastersystem\"],\"dir\":\"mastersystem\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"gamegear\",\"gg\"],\"dir\":\"gamegear\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"segacd\",\"sega-cd\"],\"dir\":\"segacd\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"sg1000\"],\"dir\":\"sg1000\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"arcade\",\"fbneo\",\"neogeoaes\",\"neogeomvs\",\"cps1\",\"cps2\",\"cps3\"],\"dir\":\"fbneo\",\"emulator\":\"fbneo\",\"core_name\":\"FinalBurn Neo\"},"
    /* Saves made of several files, synced as one zip per game. */
    "{\"romm\":[\"ps2\"],\"dir\":\"ps2\",\"emulator\":\"pcsx2\",\"core_name\":\"LRPS2\",\"save_exts\":\".ps2\","
    "\"memcards\":\"{bios}/pcsx2/memcards/Mcd001.ps2,{bios}/pcsx2/memcards/Mcd002.ps2\","
    "\"cards\":{\"default\":\"shared\",\"modes\":\"per_game,shared\",\"change\":\"Turn off Shared Memory Cards in LRPS2's core options\","
    "\"setting\":{\"core_option\":\"pcsx2_shared_memory_cards\",\"per_game\":\"disabled\",\"shared\":\"enabled\",\"default\":\"enabled\"}}},"
    "{\"romm\":[\"psp\"],\"dir\":\"psp\",\"emulator\":\"ppsspp\",\"core_name\":\"PPSSPP\",\"save_layout\":\"psp\"},"
    "{\"romm\":[\"ngc\",\"gamecube\"],\"dir\":\"gamecube\",\"emulator\":\"dolphin\",\"core_name\":\"Dolphin\",\"save_layout\":\"gci\"},"
    "{\"romm\":[\"wii\"],\"dir\":\"wii\",\"emulator\":\"dolphin\",\"core_name\":\"Dolphin\",\"save_layout\":\"wii\"}"
    "]},"
    "{\"id\":\"mednafen\",\"name\":\"Mednafen\",\"enabled\":false,"
    "\"root\":\"/data/homebrew/Mednafen\","
    "\"rom_dir\":\"{root}/roms\","
    "\"save_dir\":\"{root}/sav\","
    "\"state_dir\":\"{root}/mcs\","
    "\"bios_dir\":\"{root}/firmware\","
    "\"save_exts\":\".sav\","
    "\"state_exts\":\".mc*\","
    "\"save_name\":\"{rom_stem}.{rom_md5}{ext}\","
    "\"sync_saves\":true,"
    "\"platforms\":["
    "{\"romm\":[\"psx\",\"ps1\"],\"emulator\":\"mednafen_psx\"},"
    "{\"romm\":[\"nes\",\"famicom\"],\"emulator\":\"mednafen_nes\"},"
    "{\"romm\":[\"snes\",\"sfam\"],\"emulator\":\"mednafen_snes\"},"
    "{\"romm\":[\"gb\",\"gbc\"],\"emulator\":\"mednafen_gb\"},"
    "{\"romm\":[\"gba\"],\"emulator\":\"mednafen_gba\"},"
    "{\"romm\":[\"genesis\",\"genesis-slash-megadrive\",\"megadrive\",\"sms\",\"gamegear\"],\"emulator\":\"mednafen_md\"},"
    "{\"romm\":[\"tg16\",\"turbografx16--1\",\"pce\",\"pcengine\"],\"emulator\":\"mednafen_pce\"},"
    "{\"romm\":[\"ngp\",\"ngpc\",\"neo-geo-pocket\",\"neo-geo-pocket-color\"],\"emulator\":\"mednafen_ngp\"},"
    "{\"romm\":[\"wonderswan\",\"wonderswan-color\",\"ws\",\"wsc\"],\"emulator\":\"mednafen_wswan\"},"
    "{\"romm\":[\"virtualboy\",\"vb\"],\"emulator\":\"mednafen_vb\"},"
    "{\"romm\":[\"lynx\"],\"emulator\":\"mednafen_lynx\"}"
    "]},"
    "{\"id\":\"ps2-classics\",\"name\":\"PS2 ISOs (for PS2 FPKG tools)\",\"enabled\":false,"
    "\"root\":\"/mnt/usb0\",\"rom_dir\":\"{root}/ps2\",\"bios_dir\":\"{root}/ps2/bios\","
    "\"sync_saves\":false,\"sync_note\":\"PS2 games made into packages keep their saves in the console's own save data.\","
    "\"platforms\":[{\"romm\":[\"ps2\"],\"emulator\":\"ps2-classics\"}]}"
    "]";


static const char *jstr(const cJSON *a, const cJSON *b, const char *key, const char *def) {
    const cJSON *v = b ? cJSON_GetObjectItemCaseSensitive(b, key) : NULL;
    if (!cJSON_IsString(v)) v = cJSON_GetObjectItemCaseSensitive(a, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

cJSON *profiles_default(void) {
    cJSON *defs = cJSON_Parse(DEFAULT_PROFILES_JSON), *p;
    const plat_info_t *pi = plat_info();
    cJSON_ArrayForEach(p, defs) {
        const char *id = jstr(p, NULL, "id", "");
        const char *root = !strcmp(id, "retroarch") ? pi->retroarch_root : !strcmp(id, "mednafen") ? pi->mednafen_root : NULL;
        if (root) cJSON_ReplaceItemInObjectCaseSensitive(p, "root", cJSON_CreateString(root));
    }
    return defs;
}

/* The built-in catalog's setting for the same emulator and system: its
 * platform's, else its profile's. *known says whether the catalog has the
 * emulator at all (RetroArch and hand-made profiles aren't in it). */
static const cJSON *catalog_setting(const cJSON *p, const cJSON *e, const char *key, int *known) {
    const cJSON *cat = cJSON_GetObjectItemCaseSensitive(detect_catalog_entry(jstr(p, NULL, "id", "")), "profile"), *ce, *v;
    if (known) *known = cat != NULL;
    if (!cat) return NULL;
    cJSON_ArrayForEach(ce, cJSON_GetObjectItemCaseSensitive(cat, "platforms")) {
        if (strcmp(profiles_system_key(ce), profiles_system_key(e)) != 0) continue;
        if ((v = cJSON_GetObjectItemCaseSensitive(ce, key))) return v;
    }
    return cJSON_GetObjectItemCaseSensitive(cat, key);
}

/* A memory card setting: the catalog's for an emulator it has (cards follow
 * the emulator's versions, as sync_saves does), else the profile's own. */
static const cJSON *card_setting(const cJSON *p, const cJSON *e, const char *key) {
    int known = 0;
    const cJSON *v = catalog_setting(p, e, key, &known);
    if (known && v) return v;
    v = cJSON_GetObjectItemCaseSensitive(e, key);
    return v ? v : cJSON_GetObjectItemCaseSensitive(p, key);
}

static const char *card_str(const cJSON *p, const cJSON *e, const char *key, const char *def) {
    const cJSON *v = card_setting(p, e, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

void profile_card_dir(const profile_map *m, char *out, size_t n) {
    char list[sizeof m->pc_dirs], *save = NULL;
    str_copy(list, sizeof list, m->pc_dirs);
    out[0] = 0;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        str_copy(out, n, t); /* the last one when none exists */
        if (dir_exists(t)) return;
    }
}

/* The title id an app folder (<homebrew>/PPSA99169) is for, or 0. The
 * folders apps are found in are detect.c's (ROMM_SYNC_HOMEBREW in tests). */
static int app_title_of(const char *dir, char *title, size_t tn) {
    const char *const *hb = plat_info()->homebrew_dirs, *env = getenv("ROMM_SYNC_HOMEBREW");
    for (int i = -1; i < 0 || (hb && hb[i]); i++) {
        const char *base = i < 0 ? env : hb[i];
        if (!base || !*base) continue;
        size_t l = strlen(base);
        if (strncmp(dir, base, l) != 0 || dir[l] != '/' || strlen(dir + l + 1) < 9 || tn < 10) continue;
        const char *t = dir + l + 1;
        if (t[9] && t[9] != '/') continue;
        for (int k = 0; k < 9; k++)
            if (k < 4 ? !(t[k] >= 'A' && t[k] <= 'Z') : !(t[k] >= '0' && t[k] <= '9')) return 0;
        memcpy(title, t, 9);
        title[9] = 0;
        return 1;
    }
    return 0;
}

/* A save state setting: the profile's own, else the built-in catalog's for
 * the same emulator and system (profiles saved before these settings existed). */
static const cJSON *state_setting(const cJSON *p, const cJSON *e, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, key);
    if (!v) v = cJSON_GetObjectItemCaseSensitive(p, key);
    return v ? v : catalog_setting(p, e, key, NULL);
}

static const char *state_str(const cJSON *p, const cJSON *e, const char *key, const char *def) {
    const cJSON *v = state_setting(p, e, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static int jbool(const cJSON *a, const cJSON *b, const char *key, int def) {
    const cJSON *v = b ? cJSON_GetObjectItemCaseSensitive(b, key) : NULL;
    if (!cJSON_IsBool(v)) v = cJSON_GetObjectItemCaseSensitive(a, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

/* Minimal retroarch.cfg reader: key = "value" */
static int ra_cfg_get(const char *cfg, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *line = cfg; line && *line;) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, key, kl) && (line[kl] == ' ' || line[kl] == '\t' || line[kl] == '=')) {
            const char *q = strchr(line, '"');
            const char *eol = strchr(line, '\n');
            if (q && (!eol || q < eol)) {
                const char *e = strchr(q + 1, '"');
                if (e) {
                    size_t l = (size_t)(e - q - 1);
                    if (l >= n) l = n - 1;
                    memcpy(out, q + 1, l);
                    out[l] = 0;
                    return 1;
                }
            }
            /* An ini's unquoted value (Flycast's emu.cfg: "PerGameVmu = yes"). */
            const char *eq = strchr(line, '=');
            if (eq && (!eol || eq < eol)) {
                const char *v = eq + 1, *end = eol ? eol : v + strlen(v);
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
                size_t l = (size_t)(end - v);
                if (l >= n) l = n - 1;
                memcpy(out, v, l);
                out[l] = 0;
                return 1;
            }
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return 0;
}

/* "" and "default" mean unset; ":", "~" and "/app0" all mean the app folder. */
static void ra_apply(const char *cfg, const char *key, const char *root, char *dst, size_t n) {
    char v[PATH_MAX_LEN];
    if (!ra_cfg_get(cfg, key, v, sizeof v) || !v[0] || !strcmp(v, "default")) return;
    if (v[0] == ':' || v[0] == '~')
        snprintf(dst, n, "%s%s", root, v + 1);
    else if (!strncmp(v, "/app0", 5) && (v[5] == '/' || !v[5]))
        snprintf(dst, n, "%s%s", root, v + 5);
    else
        str_copy(dst, n, v);
    size_t l = strlen(dst);
    while (l > 1 && dst[l - 1] == '/') dst[--l] = 0;
}

/* Reads a core option from <config>/<core>/<core>.opt, then from the global
 * retroarch-core-options.cfg. Returns 0 if it isn't set in either. */
static const char *RA_FLAG_KEYS[2][3] = {
    {"savefiles_in_content_dir", "sort_savefiles_by_content_enable", "sort_savefiles_enable"},
    {"savestates_in_content_dir", "sort_savestates_by_content_enable", "sort_savestates_enable"}};

/* Applies the folder settings a RetroArch config (or override) sets. Keys it
 * doesn't mention keep their current value. */
static void apply_ra_cfg(const char *cfg, const char *root, const int explicit_dir[2], char *save, char *state,
                         size_t n, int in_content[2], int by_content[2], int by_core[2]) {
    char flag[16];
    if (!explicit_dir[0]) ra_apply(cfg, "savefile_directory", root, save, n);
    if (!explicit_dir[1]) ra_apply(cfg, "savestate_directory", root, state, n);
    for (int k = 0; k < 2; k++) {
        if (explicit_dir[k]) continue;
        if (ra_cfg_get(cfg, RA_FLAG_KEYS[k][0], flag, sizeof flag)) in_content[k] = !strcmp(flag, "true");
        if (ra_cfg_get(cfg, RA_FLAG_KEYS[k][1], flag, sizeof flag)) by_content[k] = !strcmp(flag, "true");
        if (ra_cfg_get(cfg, RA_FLAG_KEYS[k][2], flag, sizeof flag)) by_core[k] = !strcmp(flag, "true");
    }
}

/* Game name and content folder name RetroArch uses for override files. */
static void override_names(const char *rom_path, char *game, size_t gn, char *cdir, size_t cn) {
    char parent[PATH_MAX_LEN];
    if (dir_exists(rom_path))
        str_copy(game, gn, path_basename(rom_path));
    else
        path_stem(game, gn, rom_path);
    str_copy(parent, sizeof parent, rom_path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    str_copy(cdir, cn, path_basename(parent));
}

/* A core option, looked up the way RetroArch does: per game, per content
 * folder, per core, then the global options file. */
static int core_option(const profile_map *m, const char *key, const char *game, const char *cdir, char *out, size_t n) {
    const char *names[3] = {game, cdir, m->core_name};
    char path[PATH_MAX_LEN];
    for (int i = 0; i < 4; i++) {
        if (i < 3) {
            if (!names[i] || !names[i][0] || !m->core_name[0]) continue;
            snprintf(path, sizeof path, "%s/%s/%s.opt", m->ra_cfg_dir, m->core_name, names[i]);
        } else {
            snprintf(path, sizeof path, "%s/retroarch-core-options.cfg", m->ra_cfg_dir);
        }
        char *txt = read_file(path, NULL);
        int found = txt && ra_cfg_get(txt, key, out, n);
        free(txt);
        if (found) return 1;
    }
    return 0;
}

/* val is one of the comma separated list's values. */
static int in_list_ci(const char *val, const char *list) {
    char buf[256], *save = NULL;
    str_copy(buf, sizeof buf, list);
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        if (str_ieq(t, val)) return 1;
    return 0;
}

/* Which memory cards the emulator keeps now, "per_game" or "shared", from
 * the setting the platform's "cards" names (a core option, or a line of the
 * emulator's own settings file), else its default. */
static void cards_mode_now(profile_map *m) {
    char val[96] = "";
    int found = 0;
    if (m->cards_option[0] && m->ra_cfg_dir[0]) found = core_option(m, m->cards_option, NULL, NULL, val, sizeof val);
    else if (m->cards_file[0] && m->cards_key[0]) {
        char *txt = read_file(m->cards_file, NULL);
        found = txt && ra_cfg_get(txt, m->cards_key, val, sizeof val);
        free(txt);
    }
    str_copy(m->cards_mode, sizeof m->cards_mode, m->cards_default);
    if (found && in_list_ci(val, m->cards_per_game)) str_copy(m->cards_mode, sizeof m->cards_mode, "per_game");
    else if (found && in_list_ci(val, m->cards_shared)) str_copy(m->cards_mode, sizeof m->cards_mode, "shared");
}

static void check_requirement(const profile_map *m, const char *game, const char *cdir, int *sync, char *note,
                              size_t nn) {
    *sync = m->base_sync_saves;
    note[0] = 0;
    if (m->sync_off) str_copy(note, nn, "Save sync is off for this platform.");
    else if (!m->base_sync_saves) str_copy(note, nn, m->off_note);
    if (!m->req_key[0] || !m->base_sync_saves) return;
    char val[64];
    if (!m->ra_cfg_dir[0] || !core_option(m, m->req_key, game, cdir, val, sizeof val)) str_copy(val, sizeof val, m->req_default);
    if (in_list_ci(val, m->req_value)) return;
    *sync = 0;
    if (m->req_hint[0])
        str_copy(note, nn, m->req_hint);
    else
        snprintf(note, nn, "saves sync when core option %s is \"%s\" (now \"%s\")", m->req_key, m->req_value, val);
}

int profile_ra_dir(const profile_map *m, const char *key, char *out, size_t n) {
    char path[PATH_MAX_LEN];
    out[0] = 0;
    if (!m->ra_cfg_dir[0]) return 0;
    path_join(path, sizeof path, m->ra_cfg_dir, "retroarch.cfg");
    char *cfg = read_file(path, NULL);
    if (cfg) ra_apply(cfg, key, m->root, out, n);
    free(cfg);
    return out[0] != 0;
}

void profile_rules_for(const profile_map *m, const char *rom_path, rom_rules *out) {
    memset(out, 0, sizeof *out);
    str_copy(out->save_dir, sizeof out->save_dir, m->save_dir);
    str_copy(out->state_dir, sizeof out->state_dir, m->state_dir);
    memcpy(out->in_content, m->in_content, sizeof out->in_content);
    memcpy(out->by_content, m->by_content, sizeof out->by_content);
    memcpy(out->by_core, m->by_core, sizeof out->by_core);
    out->sync_saves = m->sync_saves;
    str_copy(out->sync_note, sizeof out->sync_note, m->sync_note);
    if (!rom_path || !m->ra_cfg_dir[0] || !m->core_name[0]) return;

    char game[256], cdir[256];
    override_names(rom_path, game, sizeof game, cdir, sizeof cdir);
    const char *names[3] = {m->core_name, cdir, game};
    for (int i = 0; i < 3; i++) {
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof path, "%s/%s/%s.cfg", m->ra_cfg_dir, m->core_name, names[i]);
        char *cfg = read_file(path, NULL);
        if (!cfg) continue;
        apply_ra_cfg(cfg, m->root, m->explicit_dir, out->save_dir, out->state_dir, sizeof out->save_dir,
                     out->in_content, out->by_content, out->by_core);
        free(cfg);
    }
    check_requirement(m, game, cdir, &out->sync_saves, out->sync_note, sizeof out->sync_note);
}

static void slugs_join(const cJSON *arr, char *out, size_t n) {
    out[0] = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, arr) {
        if (!cJSON_IsString(s)) continue;
        if (out[0]) strncat(out, ",", n - strlen(out) - 1);
        strncat(out, s->valuestring, n - strlen(out) - 1);
    }
}

const char *profiles_system_key(const cJSON *entry) {
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(entry, "romm"), 0);
    return cJSON_IsString(first) ? first->valuestring : "";
}

const char *profiles_save_family(const char *format) {
    return !strcmp(format, "nds-dsv") ? "nds-raw" : format;
}

/* A profile's platform entries, with a twin for each of RetroArch's other
 * cores per system ("cores") whose rule names a save format: RetroArch picks
 * the core as a game starts, and a core reads its save before anyone can
 * react, so each one's save is kept. Only cores keeping it in the entry's
 * own core's format (or a form of it): the others would each need a slot. */
static cJSON *with_core_twins(const cJSON *p) {
    static const char *const keep[] = {"romm", "dir", "db", "rom_dir", "update_dir", "dlc_dir", "bios_dir", "sync"};
    cJSON *plats = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(p, "platforms"), 1);
    if (!plats) plats = cJSON_CreateArray();
    const int n = cJSON_GetArraySize(plats);
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(plats, i), *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(e, "cores")) {
            const char *core = jget_str(c, "core_name", "");
            if (!core[0] || str_ieq(core, jstr(e, NULL, "core_name", ""))) continue;
            cJSON *t = cJSON_CreateObject();
            for (size_t k = 0; k < sizeof keep / sizeof *keep; k++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, keep[k]);
                if (v) cJSON_AddItemToObject(t, keep[k], cJSON_Duplicate(v, 1));
            }
            cJSON_AddStringToObject(t, "emulator", jget_str(c, "emulator", core));
            cJSON_AddStringToObject(t, "core_name", core);
            cJSON_AddBoolToObject(t, "twin", 1);
            detect_apply_core_rules(t, profiles_system_key(e), core, 0);
            /* One save between them: the same family as the entry's own core. */
            const char *mine = jstr(e, NULL, "save_format", ""), *its = jstr(t, NULL, "save_format", "");
            if (mine[0] && its[0] && !strcmp(profiles_save_family(mine), profiles_save_family(its))) cJSON_AddItemToArray(plats, t);
            else cJSON_Delete(t);
        }
    }
    return plats;
}

/* Whether a profile's list ("exclude", "extra") names a system. */
static int profile_lists(const cJSON *profile, const char *list, const char *key) {
    const cJSON *x;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(profile, list))
        if (cJSON_IsString(x) && str_ieq(x->valuestring, key)) return 1;
    return 0;
}

int profiles_excludes(const cJSON *profile, const char *key) { return profile_lists(profile, "exclude", key); }

/* Where an app folder's path is inside the app's sandbox: a folder in
 * <homebrew>/<title>/ is <sandbox>/app0/... ("PPSA99248", "app0/games"). */
static int app0_path(const char *dir, char *title, size_t tn, char *into, size_t in) {
    const char *const *hb = plat_info()->homebrew_dirs;
    for (int i = 0; hb && hb[i]; i++) {
        size_t l = strlen(hb[i]);
        if (strncmp(dir, hb[i], l) != 0 || dir[l] != '/') continue;
        const char *t = dir + l + 1, *rest = strchr(t, '/');
        if (!rest || rest - t != 9 || !rest[1] || tn < 10) return 0;
        for (int k = 0; k < 9; k++)
            if (k < 4 ? !(t[k] >= 'A' && t[k] <= 'Z') : !(t[k] >= '0' && t[k] <= '9')) return 0;
        memcpy(title, t, 9);
        title[9] = 0;
        return snprintf(into, in, "app0%s", rest) < (int)in;
    }
    return 0;
}

/* An extra with "mount_games" plays the main emulator's games through a mount
 * in its sandbox, so they aren't copied (bfs has no hard links). Not when the
 * main one's folder holds other systems' games too (Porpoise: GameCube and
 * Wii), or the extra's own folder has games that aren't the main one's: the
 * mount would hide them. Copies the main one has too are RommPS's, from
 * before mounts (library.c clears them out). */
/* A Dolphin's GameCube or Wii save folder: the first of its candidates that's
 * there, else the first (sync.c's bundle_bases has the same defaults). */
static void dolphin_dir(const profile_map *m, char *out, size_t n) {
    static const char *gci = "{save_root}/dolphin-emu/User/GC,{save_path}/User/GC,{save_path}/dolphin-emu/User/GC,{bios}/dolphin-emu/User/GC";
    static const char *wii = "{save_root}/dolphin-emu/User/Wii,{save_path}/User/Wii,{save_path}/dolphin-emu/User/Wii,{bios}/dolphin-emu/User/Wii";
    char save_path[PATH_MAX_LEN], list[2048], *save = NULL;
    str_copy(save_path, sizeof save_path, m->save_dir);
    if (m->by_core[0] && m->core_name[0]) path_join(save_path, sizeof save_path, m->save_dir, m->core_name);
    const char *vars[] = {"save_path", save_path, "save_root", m->save_dir, "bios", m->bios_dir, "root", m->root, NULL};
    str_template(list, sizeof list, m->bundle_dirs[0] ? m->bundle_dirs : !strcmp(m->save_layout, "gci") ? gci : wii, vars);
    out[0] = 0;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!out[0]) str_copy(out, n, t);
        if (dir_exists(t)) {
            str_copy(out, n, t);
            return;
        }
    }
}

/* Dolphin's saves, kept the same way by the main emulator and an extra
 * (Porpoise and RetroArch's Dolphin core): the main's folder is mounted on the
 * extra's, so both play the same saves, synced once from the main's. Even
 * when the games can't be mounted (Porpoise keeps GameCube and Wii games in
 * one folder): the saves are each system's own. */
static void mount_main_saves(const profile_map *m, profile_map *x) {
    if (strcmp(x->save_layout, m->save_layout) != 0 || (strcmp(x->save_layout, "gci") && strcmp(x->save_layout, "wii")))
        return;
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN], title[16], into[256];
    dolphin_dir(m, from, sizeof from);
    dolphin_dir(x, to, sizeof to);
    if (!from[0] || !to[0] || !strcmp(from, to) || strlen(from) >= sizeof x->saves_from || strlen(to) >= sizeof x->saves_dir)
        return;
    if (x->mount_games == 2) { /* out of its sandbox: the real folder */
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(detect_catalog_entry(x->profile_id), "title_ids");
        const char *tid = cJSON_IsString(cJSON_GetArrayItem(ids, 0)) ? cJSON_GetArrayItem(ids, 0)->valuestring : "";
        if (strlen(tid) != 9 || snprintf(into, sizeof into, "%s", to) >= (int)sizeof into) return;
        str_copy(title, sizeof title, tid);
    } else if (!app0_path(to, title, sizeof title, into, sizeof into)) {
        return;
    }
    str_copy(x->saves_from, sizeof x->saves_from, from);
    str_copy(x->saves_dir, sizeof x->saves_dir, to);
    str_copy(x->saves_into, sizeof x->saves_into, into);
    str_copy(x->saves_title, sizeof x->saves_title, title);
    str_copy(x->bundle_dirs, sizeof x->bundle_dirs, from);
}

static void mount_main_games(profile_map *maps, int n, profile_map *x) {
    if (!x->extra || x->twin || !x->mount_games || !plat_info()->can_mount) return;
    char sys[64], title[16], into[256];
    str_copy(sys, sizeof sys, x->romm_slugs);
    sys[strcspn(sys, ",")] = 0;
    const profile_map *m = profile_find_for_platform(maps, n, sys, NULL);
    if (!m || m == x || m->extra) return;
    mount_main_saves(m, x);
    if (!strcmp(m->rom_dir, x->rom_dir)) return;
    if (x->mount_games == 2) {
        /* It leaves its sandbox and reads the real folder under /data. */
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(detect_catalog_entry(x->profile_id), "title_ids");
        const char *tid = cJSON_IsString(cJSON_GetArrayItem(ids, 0)) ? cJSON_GetArrayItem(ids, 0)->valuestring : "";
        if (strlen(tid) != 9 || strncmp(x->rom_dir, "/data/", 6) != 0 || !x->rom_dir[6]) return;
        str_copy(title, sizeof title, tid);
        if (snprintf(into, sizeof into, "%s", x->rom_dir) >= (int)sizeof into) return;
    } else if (!app0_path(x->rom_dir, title, sizeof title, into, sizeof into)) {
        return;
    }
    /* Another emulator playing the games in this one's folder (SwanStationPS5
     * in PSXS5's) would see them go while the mount is on: both keep copies. */
    for (int k = 0; k < n; k++) {
        const char *theirs = maps[k].own_rom_dir[0] ? maps[k].own_rom_dir : maps[k].rom_dir;
        /* Unless it gets the same mount there too (PSXS5 and SwanStationPS5
         * on /data/PSXS5/games, each while it runs). */
        const int mounts_too = maps[k].extra && maps[k].mount_games == x->mount_games && x->mount_games == 2;
        if (&maps[k] != x && strcmp(maps[k].profile_id, x->profile_id) != 0 && !strcmp(theirs, x->rom_dir) && !mounts_too) {
            snprintf(x->mount_note, sizeof x->mount_note, "%s plays the games in its folder too: they're copied from %s",
                     maps[k].profile_name, m->profile_name);
            return;
        }
    }
    for (int k = 0; k < n; k++)
        if (&maps[k] != m && !maps[k].twin && !strcmp(maps[k].rom_dir, m->rom_dir) && strcmp(maps[k].profile_id, m->profile_id) == 0) {
            snprintf(x->mount_note, sizeof x->mount_note, "%s keeps other systems' games in the same folder",
                     m->profile_name);
            return;
        }
    /* The extra's own folder holding its other systems' games too (Beetle PCE
     * PS5: HuCards, CDs and SuperGrafx in one): one mount there would hide
     * the others' games, and each system can't have its own. */
    for (int k = 0; k < n; k++) {
        const char *theirs = maps[k].own_rom_dir[0] ? maps[k].own_rom_dir : maps[k].rom_dir;
        if (&maps[k] != x && !maps[k].twin && !strcmp(maps[k].profile_id, x->profile_id) && !strcmp(theirs, x->rom_dir)) {
            snprintf(x->mount_note, sizeof x->mount_note, "%s keeps other systems' games in the same folder",
                     x->profile_name);
            return;
        }
    }
    /* With our mount on it (the app runs), what's there is the main one's. */
    DIR *d = plat_is_mount(x->rom_dir) ? NULL : opendir(x->rom_dir);
    struct dirent *de;
    int own = 0;
    while (d && (de = readdir(d))) {
        /* A README the emulator ships in its games folder (Crux2's) is no game. */
        if (de->d_name[0] == '.' || ext_in_list(de->d_name, ".part,.tmp") || library_not_game(de->d_name)) continue;
        char there[PATH_MAX_LEN];
        path_join(there, sizeof there, m->rom_dir, de->d_name);
        own += !file_exists(there) && !dir_exists(there);
    }
    if (d) closedir(d);
    if (own) {
        x->mount_blocked = own;
        snprintf(x->mount_note, sizeof x->mount_note, "%d game%s of its own in %s: they're copied from %s instead", own,
                 own == 1 ? "" : "s", x->rom_dir, m->profile_name);
        return;
    }
    str_copy(x->own_rom_dir, sizeof x->own_rom_dir, x->rom_dir);
    str_copy(x->rom_dir, sizeof x->rom_dir, m->rom_dir);
    str_copy(x->mount_title, sizeof x->mount_title, title);
    str_copy(x->mount_into, sizeof x->mount_into, into);
}

/* One memory card method per system, never both: the one the user chose
 * (config_card_mode) if the system's main emulator can keep it, else the one
 * the main emulator keeps now. An emulator keeping the other method syncs no
 * cards at all: shared ones aren't backed up, and its own per game aren't
 * synced, with a note saying why. */
static void card_methods(profile_map *maps, int cnt) {
    for (int i = 0; i < cnt; i++) {
        profile_map *m = &maps[i];
        if (!m->cards_mode[0]) continue;
        char sys[64];
        str_copy(sys, sizeof sys, m->romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        const profile_map *main = m;
        for (int k = 0; k < cnt; k++) {
            char sk[64];
            str_copy(sk, sizeof sk, maps[k].romm_slugs);
            sk[strcspn(sk, ",")] = 0;
            if (!maps[k].extra && !maps[k].twin && maps[k].cards_mode[0] && !strcmp(sk, sys)) {
                main = &maps[k];
                break;
            }
        }
        const char *chosen = config_card_mode(sys);
        /* An emulator with no setting to read keeps whichever cards the user chose
         * (PS5SX2: its cards whole, or each game's saves on them). */
        if (!m->cards_option[0] && !m->cards_file[0] && chosen[0] && in_list_ci(chosen, m->cards_modes))
            str_copy(m->cards_mode, sizeof m->cards_mode, chosen);
        const char *method = chosen[0] && in_list_ci(chosen, main->cards_modes) ? chosen : main->cards_mode;
        str_copy(m->cards_method, sizeof m->cards_method, method);
        m->cards_active = !strcmp(m->cards_mode, method);
        /* Keeping shared cards, a game's own card files (left from before) don't sync. */
        if (m->cards_active && !strcmp(method, "shared")) {
            m->sync_saves = m->base_sync_saves = 0;
            if (!m->sync_note[0]) str_copy(m->sync_note, sizeof m->sync_note, "Shared memory cards: synced whole, as RomM memory cards.");
        }
        if (m->cards_active) continue;
        if (!strcmp(method, "shared")) {
            m->sync_saves = m->base_sync_saves = 0;
            str_copy(m->sync_note, sizeof m->sync_note,
                     "Its memory cards don't sync: it keeps one per game, and this system uses shared cards.");
        } else if (m->cards_change[0]) {
            snprintf(m->sync_note, sizeof m->sync_note, "Its memory cards don't sync: they're shared, and this system uses one per game. %s.",
                     m->cards_change);
        } else {
            str_copy(m->sync_note, sizeof m->sync_note,
                     "Its memory cards don't sync: they're shared, and this system uses one per game.");
        }
    }
}

/* The table lives in memory mapped for it, outside malloc's pool: the PS4's
 * payload shares that pool with GoldHEN, and once it runs low a block of a
 * couple of MB can't be found in it while mapped memory still can. Its size
 * is kept in front of it, for profiles_free. */
#define MAPS_HEAD 64

void profiles_free(profile_map *maps) {
    if (!maps) return;
    unsigned char *base = (unsigned char *)maps - MAPS_HEAD;
    size_t bytes;
    memcpy(&bytes, base, sizeof bytes);
    munmap(base, bytes);
}

/* Room for count entries, the first used of them copied over. */
static profile_map *maps_grow(profile_map *old, size_t used, size_t count) {
    const size_t bytes = MAPS_HEAD + sizeof *old * count;
    unsigned char *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) return NULL;
    memcpy(base, &bytes, sizeof bytes);
    profile_map *maps = (profile_map *)(base + MAPS_HEAD);
    if (old) {
        memcpy(maps, old, sizeof *old * used);
        profiles_free(old);
    }
    return maps;
}

/* A Switch emulator's user whose saves sync: the one the profile names
 * ("switch_user"), else its first (ProsperoEden, and PS5-ECAM's Eden, keep
 * profiles.first in their config: "switch_users"). That user's saves are left
 * alone when the emulator syncs them itself ("switch_sync": ProsperoEden's
 * save-sync.json gives the user a server). Never another user's. */
static cJSON *read_json(const char *path) {
    char *txt = path[0] ? read_file(path, NULL) : NULL;
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    return j;
}

static void switch_user_for(profile_map *m, const cJSON *p, const cJSON *e, const char **vars) {
    char path[PATH_MAX_LEN];
    str_copy(m->switch_user, sizeof m->switch_user, jstr(p, e, "switch_user", ""));
    if (!m->switch_user[0]) {
        str_template(path, sizeof path, jstr(p, e, "switch_users", ""), vars);
        cJSON *cfg = read_json(path);
        str_copy(m->switch_user, sizeof m->switch_user,
                 jget_str(cJSON_GetObjectItemCaseSensitive(cfg, "profiles"), "first", ""));
        cJSON_Delete(cfg);
    }
    if (!m->switch_user[0]) {
        m->sync_saves = m->base_sync_saves = 0;
        str_copy(m->off_note, sizeof m->off_note, "No Switch user in the emulator yet: open it and make one.");
        str_copy(m->sync_note, sizeof m->sync_note, m->off_note);
        return;
    }
    str_template(path, sizeof path, jstr(p, e, "switch_sync", ""), vars);
    cJSON *own = read_json(path);
    const cJSON *u;
    cJSON_ArrayForEach(u, cJSON_GetObjectItemCaseSensitive(own, "profiles"))
        if (!strcmp(jget_str(u, "profile", ""), m->switch_user) && jget_str(u, "url", "")[0]) {
            m->sync_saves = m->base_sync_saves = 0;
            snprintf(m->off_note, sizeof m->off_note, "%s syncs this user's saves itself.", m->profile_name);
            str_copy(m->sync_note, sizeof m->sync_note, m->off_note);
        }
    cJSON_Delete(own);
}

/* An Xbox 360 emulator's player whose saves sync: the one the profile names
 * ("x360_user"), else the first signed in (PS5X360's local-players.txt, one
 * XUID per player slot, 0 for none: "x360_users"). Its saves folder is the
 * XUID in capitals, as Xenia names it. Never another player's. */
static void x360_user_for(profile_map *m, const cJSON *p, const cJSON *e, const char **vars) {
    char path[PATH_MAX_LEN];
    str_copy(m->xuid, sizeof m->xuid, jstr(p, e, "x360_user", ""));
    str_template(path, sizeof path, jstr(p, e, "x360_users", ""), vars);
    char *txt = !m->xuid[0] && path[0] ? read_file(path, NULL) : NULL, *save = NULL;
    for (char *l = txt ? strtok_r(txt, "\r\n", &save) : NULL; l && !m->xuid[0]; l = strtok_r(NULL, "\r\n", &save)) {
        size_t n = 0;
        while (isxdigit((unsigned char)l[n])) n++;
        if (n == 16 && !l[n] && strspn(l, "0") != 16) str_copy(m->xuid, sizeof m->xuid, l);
    }
    free(txt);
    for (char *c = m->xuid; *c; c++) *c = (char)toupper((unsigned char)*c);
    if (!m->xuid[0]) {
        m->sync_saves = m->base_sync_saves = 0;
        snprintf(m->off_note, sizeof m->off_note, "No player profile in %s yet: open it and make one.", m->profile_name);
        str_copy(m->sync_note, sizeof m->sync_note, m->off_note);
    }
}

/* Where an emulator (or one of its systems) keeps its own files: the folder
 * of "user_dir" with "user_dir_has" in it, else the first that exists, else
 * the last. PS5-ECAM 5.1 marks each system it moved out of /data/ps5cemu
 * (when the user says yes, and not for one that couldn't move), and leaves
 * empty folders behind for others. 1 when the mark picked it. */
static int pick_user_dir(char *out, size_t n, const cJSON *o, const char *root) {
    const char *uvars[] = {"root", root, NULL};
    const char *has = jstr(o, NULL, "user_dir_has", "");
    char list[PATH_MAX_LEN], probe[PATH_MAX_LEN], there[PATH_MAX_LEN] = "", *save = NULL;
    str_template(list, sizeof list, jstr(o, NULL, "user_dir", "{root}"), uvars);
    out[0] = 0;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        str_copy(out, n, t);
        if (has[0]) {
            path_join(probe, sizeof probe, t, has);
            if (dir_exists(probe) || file_size(probe) >= 0) return 1;
        }
        if (!there[0] && dir_exists(t)) str_copy(there, sizeof there, t);
    }
    if (there[0]) str_copy(out, n, there);
    return 0;
}

/* The same file or folder under two names (a "//", a mount showing one in
 * the other): never a copy of itself to delete. */
static int same_node(const char *a, const char *b) {
    struct stat x, y;
    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

/* Each file of old that now has the same of goes, then the folders left
 * empty. Anything only old has stays, and counts in kept. */
static void clear_left_in(const char *old, const char *now, int *gone, int64_t *bytes, int *kept) {
    DIR *d = opendir(old);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char a[PATH_MAX_LEN], b[PATH_MAX_LEN], ma[33], mb[33];
        path_join(a, sizeof a, old, de->d_name);
        path_join(b, sizeof b, now, de->d_name);
        if (same_node(a, b)) {
            (*kept)++;
            continue;
        }
        if (dir_exists(a)) {
            if (plat_is_mount(a)) (*kept)++;
            else {
                clear_left_in(a, b, gone, bytes, kept);
                rmdir(a); /* (not when something stayed) */
            }
            continue;
        }
        const int64_t size = file_size(a);
        if (size >= 0 && size == file_size(b) && md5_file_hex(a, ma) == 0 && md5_file_hex(b, mb) == 0 && !strcmp(ma, mb) &&
            unlink(a) == 0) {
            (*gone)++;
            *bytes += size;
        } else {
            (*kept)++;
        }
    }
    closedir(d);
}

static int path_under(const char *path, const char *dir) {
    const size_t n = strlen(dir);
    return !strncmp(path, dir, n) && path[n] == '/';
}

void profiles_follow_moves(const cJSON *profiles) {
    const cJSON *p, *e;
    cJSON_ArrayForEach(p, profiles) {
        if (!jbool(p, NULL, "enabled", 1)) continue;
        const char *root = jstr(p, NULL, "root", ""), *name = jstr(p, NULL, "name", "");
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
            if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "user_dir"))) continue;
            char now[PATH_MAX_LEN], list[PATH_MAX_LEN], *save = NULL;
            /* Only once the emulator's mark says the system moved: a move
             * put off, undone or failed leaves its folders where they were. */
            if (!pick_user_dir(now, sizeof now, e, root)) continue;
            const char *uvars[] = {"root", root, NULL};
            str_template(list, sizeof list, jstr(e, NULL, "user_dir", ""), uvars);
            for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                if (!strcmp(t, now) || path_under(t, now) || path_under(now, t) || same_node(t, now)) continue;
                /* Its games', saves' and states' records go along, or they'd
                 * look new to RomM: gone through the emulator's own move too.
                 * By the system's own folders, so a move out of the
                 * emulator's root (Wii U's) takes only that system's. */
                int moved = 0;
                static const char *const keys[] = {"rom_dir", "save_dir", "state_dir", "bundle_dirs"};
                for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
                    const char *tpl = jstr(e, NULL, keys[k], jstr(p, NULL, keys[k], ""));
                    if (!strstr(tpl, "{user_dir}")) continue;
                    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
                    const char *fv[] = {"root", root, "user_dir", t, "platform", jstr(e, NULL, "dir", ""), NULL};
                    const char *tv[] = {"root", root, "user_dir", now, "platform", jstr(e, NULL, "dir", ""), NULL};
                    str_template(from, sizeof from, tpl, fv);
                    str_template(to, sizeof to, tpl, tv);
                    from[strcspn(from, "{")] = to[strcspn(to, "{")] = 0; /* up to {switch_user}... */
                    char *fs = strrchr(from, '/'), *ts = strrchr(to, '/');
                    if (strchr(tpl, '{') != strstr(tpl, "{user_dir}") || !fs || !ts) continue;
                    if (strstr(tpl + strlen("{user_dir}"), "{")) *fs = *ts = 0; /* the part before a variable */
                    if (!from[0] || !strcmp(from, to)) continue;
                    state_lock();
                    moved += state_rekey_moved(from, to);
                    state_unlock();
                }
                if (moved) {
                    state_lock();
                    state_save();
                    state_unlock();
                    LOGI("%s: %d record%s of %s follow it to %s", name, moved, moved == 1 ? "" : "s", t, now);
                }
                /* The emulator's own folder (its settings stay there) is never cleared. */
                if (!strcmp(t, root) || same_node(t, root)) continue;
                if (!dir_exists(t) || plat_is_mount(t)) continue;
                int gone = 0, kept = 0;
                int64_t bytes = 0;
                clear_left_in(t, now, &gone, &bytes, &kept);
                if (rmdir(t) == 0)
                    LOGI("removed %s: %s keeps it in %s now (%d copies, %lld MB)", t, name, now, gone, (long long)(bytes >> 20));
                else if (kept)
                    LOGW("%s keeps %s in %s now; %d file%s only in %s stayed", name, profiles_system_key(e), now, kept,
                         kept == 1 ? "" : "s", t);
            }
        }
    }
}

int profiles_expand(const cJSON *profiles, profile_map **out) {
    int cap = 0, cnt = 0;
    profile_map *maps = NULL;
    const cJSON *p;
    cJSON_ArrayForEach(p, profiles) {
        if (!jbool(p, NULL, "enabled", 1)) continue;
        char root[PATH_MAX_LEN], user_dir[PATH_MAX_LEN];
        str_copy(root, sizeof root, jstr(p, NULL, "root", ""));
        pick_user_dir(user_dir, sizeof user_dir, p, root);

        char ra_save[PATH_MAX_LEN] = "", ra_state[PATH_MAX_LEN] = "", ra_sys[PATH_MAX_LEN] = "";
        int ra_in_content[2] = {0, 0}, ra_by_content[2] = {0, 0}, ra_by_core[2] = {0, 0};
        char ra_cfg_dir[PATH_MAX_LEN] = "";
        const char *ra_tpl = jstr(p, NULL, "retroarch_cfg", NULL);
        if (ra_tpl) {
            /* The first config file that exists wins. */
            const char *vars[] = {"root", root, "data", plat_data_dir(), NULL};
            char list[PATH_MAX_LEN], *save = NULL;
            str_template(list, sizeof list, ra_tpl, vars);
            for (char *ra_path = strtok_r(list, ",", &save); ra_path; ra_path = strtok_r(NULL, ",", &save)) {
                char *cfg = read_file(ra_path, NULL);
                if (!cfg) continue;
                static const int none[2] = {0, 0};
                str_copy(ra_cfg_dir, sizeof ra_cfg_dir, ra_path);
                char *slash = strrchr(ra_cfg_dir, '/');
                if (slash) *slash = 0;
                apply_ra_cfg(cfg, root, none, ra_save, ra_state, sizeof ra_save, ra_in_content, ra_by_content, ra_by_core);
                ra_apply(cfg, "system_directory", root, ra_sys, sizeof ra_sys);
                free(cfg);
                break;
            }
        }

        cJSON *plats = with_core_twins(p);
        const cJSON *e;
        cJSON_ArrayForEach(e, plats) {
            if (profiles_excludes(p, profiles_system_key(e))) continue; /* another emulator plays it */
            if (cnt == cap) {
                /* Room for every entry at once: each is 13 KB, and the PS4's
                 * payload can't always find room to grow a big array. */
                int want = 0;
                const cJSON *q, *f;
                cJSON_ArrayForEach(q, profiles) {
                    if (!jbool(q, NULL, "enabled", 1)) continue;
                    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(q, "platforms"))
                        want += !profiles_excludes(q, profiles_system_key(f));
                }
                /* Short of memory for a moment (the PS4 payload, while downloads
                 * finish together): it frees up, so wait for it a little. */
                const size_t count = (size_t)(want > cap ? want : cap + 1);
                profile_map *grown = maps_grow(maps, (size_t)cnt, count);
                for (int tries = 1; !grown && tries < 10; tries++) {
                    usleep(200 * 1000);
                    grown = maps_grow(maps, (size_t)cnt, count);
                    if (grown) LOGI("memory for %d emulator platforms after %d tries", want, tries + 1);
                }
                if (!grown) {
                    LOGE("no memory for %d emulator platforms (largest free block %d MB)", want, mem_largest_mb());
                    profiles_free(maps);
                    cJSON_Delete(plats);
                    *out = NULL;
                    return 0;
                }
                maps = grown;
                cap = want > cap ? want : cap + 1;
            }
            profile_map *m = &maps[cnt];
            memset(m, 0, sizeof *m);
            str_copy(m->profile_id, sizeof m->profile_id, jstr(p, NULL, "id", "custom"));
            str_copy(m->profile_name, sizeof m->profile_name, jstr(p, NULL, "name", m->profile_id));
            str_copy(m->emulator, sizeof m->emulator, jstr(p, e, "emulator", m->profile_id));
            slugs_join(cJSON_GetObjectItemCaseSensitive(e, "romm"), m->romm_slugs,
                       sizeof m->romm_slugs);
            if (!m->romm_slugs[0]) continue;
            char first[64];
            str_copy(first, sizeof first, m->romm_slugs);
            char *comma = strchr(first, ',');
            if (comma) *comma = 0;
            str_copy(m->platform_dir, sizeof m->platform_dir, jstr(p, e, "dir", first));

            /* A system kept in a folder of its own (PS5-ECAM's: /data/melonds). */
            char sys_dir[PATH_MAX_LEN];
            if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(e, "user_dir"))) pick_user_dir(sys_dir, sizeof sys_dir, e, root);
            else str_copy(sys_dir, sizeof sys_dir, user_dir);
            const char *vars[] = {"root", root, "user_dir", sys_dir, "platform", m->platform_dir, "data",
                                  plat_data_dir(), "rom_dir", NULL, NULL};
            str_template(m->rom_dir, sizeof m->rom_dir, jstr(p, e, "rom_dir", "{root}/roms/{platform}"), vars);
            vars[9] = m->rom_dir;
            str_template(m->old_rom_dirs, sizeof m->old_rom_dirs, jstr(p, e, "old_rom_dirs", ""), vars);
            str_template(m->update_dir, sizeof m->update_dir, jstr(p, e, "update_dir", "{rom_dir}"), vars);
            str_template(m->dlc_dir, sizeof m->dlc_dir, jstr(p, e, "dlc_dir", jstr(p, e, "update_dir", "{rom_dir}")), vars);
            str_template(m->save_dir, sizeof m->save_dir, jstr(p, e, "save_dir", "{rom_dir}"), vars);
            str_template(m->state_dir, sizeof m->state_dir, jstr(p, e, "state_dir", "{rom_dir}"), vars);
            str_template(m->bios_dir, sizeof m->bios_dir, jstr(p, e, "bios_dir", "{root}/bios"), vars);
            str_template(m->n64_split_dir, sizeof m->n64_split_dir, jstr(p, e, "n64_split_dir", ""), vars);
            str_template(m->n64_ini, sizeof m->n64_ini, jstr(p, e, "n64_ini", ""), vars);
            /* The emulator's config wins unless the platform entry sets a folder. */
            if (ra_save[0] && !cJSON_GetObjectItemCaseSensitive(e, "save_dir"))
                str_copy(m->save_dir, sizeof m->save_dir, ra_save);
            if (ra_state[0] && !cJSON_GetObjectItemCaseSensitive(e, "state_dir"))
                str_copy(m->state_dir, sizeof m->state_dir, ra_state);
            if (ra_sys[0] && !cJSON_GetObjectItemCaseSensitive(e, "bios_dir"))
                str_copy(m->bios_dir, sizeof m->bios_dir, ra_sys);
            /* RetroArch's PS2 cores read their BIOS from <system>/pcsx2/bios. */
            const char *core = jstr(p, e, "core_name", "");
            const int pcsx2 = str_ieq(core, "LRPS2") || str_ieq(core, "PCSX2") || str_ieq(core, "PS5SX2");
            const char *sub = jstr(p, e, "firmware_subdir", pcsx2 && ra_tpl ? "pcsx2/bios" : "");
            str_copy(m->firmware_subdir, sizeof m->firmware_subdir, sub);
            for (int k = 0; k < 2; k++) {
                m->explicit_dir[k] = cJSON_GetObjectItemCaseSensitive(e, k ? "state_dir" : "save_dir") != NULL;
                if (m->explicit_dir[k]) continue;
                m->in_content[k] = ra_in_content[k];
                m->by_content[k] = ra_by_content[k];
                m->by_core[k] = ra_by_core[k];
            }
            str_copy(m->core_name, sizeof m->core_name, jstr(p, e, "core_name", ""));
            str_copy(m->save_layout, sizeof m->save_layout, card_str(p, e, "save_layout", ""));
            /* The rest of its variables ({switch_user}, {save_path}...) are filled in at sync. */
            const char *bvars[] = {"user_dir", sys_dir, NULL};
            str_template(m->bundle_dirs, sizeof m->bundle_dirs, jstr(p, e, "bundle_dirs", ""), bvars);

            str_copy(m->save_exts, sizeof m->save_exts, jstr(p, e, "save_exts", ".srm,.sav"));
            str_copy(m->state_exts, sizeof m->state_exts, jstr(p, e, "state_exts", ".state*"));
            str_copy(m->save_name, sizeof m->save_name, jstr(p, e, "save_name", "{rom_stem}{ext}"));
            str_copy(m->save_stems, sizeof m->save_stems, jstr(p, e, "save_stems", ""));
            str_copy(m->state_stems, sizeof m->state_stems, jstr(p, e, "state_stems", ""));
            str_copy(m->state_name, sizeof m->state_name, jstr(p, e, "state_name", "{rom_stem}{ext}"));
            str_copy(m->save_format, sizeof m->save_format, jstr(p, e, "save_format", ""));
            m->game_folders = jbool(p, e, "game_folders", 0);
            /* Every Dolphin with a GCI folder keeps GameCube saves the same way. */
            if (!m->save_format[0] && !strcmp(jstr(p, e, "save_layout", ""), "gci"))
                str_copy(m->save_format, sizeof m->save_format, "gc-gci");
            str_copy(m->state_cores, sizeof m->state_cores, state_str(p, e, "state_cores", m->emulator));
            str_copy(m->state_format, sizeof m->state_format, state_str(p, e, "state_format", "retroarch"));
            str_copy(m->state_slot0, sizeof m->state_slot0, state_str(p, e, "state_slot0", ".state"));
            str_copy(m->resume_states, sizeof m->resume_states, state_str(p, e, "resume_states", ""));
            const cJSON *nbc = state_setting(p, e, "save_name_by_core"), *nv;
            size_t no = 0;
            cJSON_ArrayForEach(nv, nbc) {
                if (!cJSON_IsString(nv)) continue;
                int w = snprintf(m->save_names_by_core + no, sizeof m->save_names_by_core - no, "%s%s=%s", no ? ";" : "",
                                 nv->string, nv->valuestring);
                if (w < 0 || (size_t)w >= sizeof m->save_names_by_core - no) break;
                no += (size_t)w;
            }
            const cJSON *cs = state_setting(p, e, "state_core_setting");
            if (cJSON_IsObject(cs)) {
                str_template(m->core_file, sizeof m->core_file, jstr(cs, NULL, "file", ""), vars);
                str_copy(m->core_key, sizeof m->core_key, jstr(cs, NULL, "key", ""));
                const cJSON *v;
                size_t o = 0;
                cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(cs, "values")) {
                    if (!cJSON_IsString(v)) continue;
                    int w = snprintf(m->core_values + o, sizeof m->core_values - o, "%s%s=%s", o ? "," : "", v->string,
                                     v->valuestring);
                    if (w < 0 || (size_t)w >= sizeof m->core_values - o) break;
                    o += (size_t)w;
                }
                str_copy(m->core_auto, sizeof m->core_auto, jstr(cs, NULL, "auto", ""));
                str_template(m->core_log, sizeof m->core_log, jstr(cs, NULL, "log", ""), vars);
                str_copy(m->core_log_line, sizeof m->core_log_line, jstr(cs, NULL, "log_line", ""));
                o = 0;
                cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(cs, "log_names")) {
                    if (!cJSON_IsString(v)) continue;
                    int w = snprintf(m->core_log_names + o, sizeof m->core_log_names - o, "%s%s=%s", o ? "," : "",
                                     v->string, v->valuestring);
                    if (w < 0 || (size_t)w >= sizeof m->core_log_names - o) break;
                    o += (size_t)w;
                }
            }
            /* Shared cards can sit in the BIOS folder RetroArch's config names
             * (Flycast's <system>/dc): {bios}. */
            const char *cvars[] = {"root", root, "user_dir", sys_dir, "bios", m->bios_dir, "data", plat_data_dir(),
                                   "card_dir", NULL, NULL};
            const cJSON *pc = card_setting(p, e, "pcsx2_cards");
            if (cJSON_IsObject(pc)) {
                str_template(m->pc_dirs, sizeof m->pc_dirs, jstr(pc, NULL, "dir", ""), cvars);
                str_template(m->pc_game_ini, sizeof m->pc_game_ini, jstr(pc, NULL, "game_ini", ""), cvars);
                str_template(m->pc_global_ini, sizeof m->pc_global_ini, jstr(pc, NULL, "global_ini", ""), cvars);
                str_copy(m->pc_format, sizeof m->pc_format, jstr(pc, NULL, "format", "flat"));
                str_copy(m->pc_own_card, sizeof m->pc_own_card, jstr(pc, NULL, "own_card", ""));
                str_copy(m->pc_auto_card, sizeof m->pc_auto_card, jstr(pc, NULL, "auto_card", ""));
                str_template(m->pc_game_db, sizeof m->pc_game_db, jstr(pc, NULL, "game_db", ""), cvars);
            }
            char card_dir[PATH_MAX_LEN];
            profile_card_dir(m, card_dir, sizeof card_dir);
            cvars[9] = card_dir;
            str_template(m->memcards, sizeof m->memcards, card_str(p, e, "memcards", ""), cvars);
            str_template(m->game_ids, sizeof m->game_ids, jstr(p, e, "game_ids", ""), vars);
            str_copy(m->disc_id, sizeof m->disc_id, jstr(p, e, "disc_id", ""));
            m->extra = profile_lists(p, "extra", profiles_system_key(e));
            m->twin = jbool(e, NULL, "twin", 0);
            /* How it reads its games was checked on the console: the preset's
             * call, as its cards are, never a saved copy's. */
            const cJSON *mg = card_setting(p, e, "mount_games");
            m->mount_games = cJSON_IsTrue(mg) ? 1 : cJSON_IsString(mg) && !strcmp(mg->valuestring, "data") ? 2 : 0;
            /* RetroArch reads its folders through its app folder (":/content",
             * ":/savefiles" in retroarch.cfg): an extra takes mounts there. */
            if (!mg && jstr(p, NULL, "retroarch_cfg", NULL)) m->mount_games = 1;
            /* Whether its saves can sync at all is the preset's call, never a
             * saved copy's: a preset turns them on once they're tested, or off
             * when an emulator update breaks them. The user's say is "sync". */
            int in_catalog = 0;
            const cJSON *can = catalog_setting(p, e, "sync_saves", &in_catalog);
            m->sync_saves = m->base_sync_saves = in_catalog ? !cJSON_IsFalse(can) : jbool(p, e, "sync_saves", 1);
            const char *mode = jstr(p, e, "sync", "all"); /* per platform, or the emulator's (before 1.2) */
            m->sync_off = !strcmp(mode, "off");
            m->sync_states = !m->sync_off && strcmp(mode, "saves") != 0;
            if (m->sync_off) m->sync_saves = m->base_sync_saves = 0;
            /* Why the preset turns them off, for the user ("sync_note"). */
            if (!m->sync_saves && !m->sync_off) {
                const cJSON *why = catalog_setting(p, e, "sync_note", &in_catalog);
                str_copy(m->off_note, sizeof m->off_note, cJSON_IsString(why) ? why->valuestring : jstr(p, e, "sync_note", ""));
            }
            str_copy(m->root, sizeof m->root, root);
            str_copy(m->ra_cfg_dir, sizeof m->ra_cfg_dir, ra_cfg_dir);
            const cJSON *tids = cJSON_GetObjectItemCaseSensitive(detect_catalog_entry(m->profile_id), "title_ids");
            if (cJSON_IsString(cJSON_GetArrayItem(tids, 0)))
                str_copy(m->app_title, sizeof m->app_title, cJSON_GetArrayItem(tids, 0)->valuestring);
            else if (!app_title_of(root, m->app_title, sizeof m->app_title))
                m->app_title[0] = 0;
            /* "cards": which memory cards the emulator keeps for the system:
             * {"default": "per_game" | "shared", "modes": "per_game,shared",
             *  "change": how the user gets one card per game, "setting": how to tell which
             *  is on: {"core_option": key} or {"file", "key"}, with the
             *  "per_game" and "shared" values (comma separated) and the
             *  core option's "default"}. */
            const cJSON *cards = card_setting(p, e, "cards");
            if (cJSON_IsObject(cards)) {
                const cJSON *set = cJSON_GetObjectItemCaseSensitive(cards, "setting");
                str_copy(m->cards_default, sizeof m->cards_default, jstr(cards, NULL, "default", "per_game"));
                str_copy(m->cards_modes, sizeof m->cards_modes, jstr(cards, NULL, "modes", m->cards_default));
                str_copy(m->cards_change, sizeof m->cards_change, jstr(cards, NULL, "change", ""));
                str_copy(m->cards_option, sizeof m->cards_option, jstr(set, NULL, "core_option", ""));
                str_template(m->cards_file, sizeof m->cards_file, jstr(set, NULL, "file", ""), vars);
                str_copy(m->cards_key, sizeof m->cards_key, jstr(set, NULL, "key", ""));
                str_copy(m->cards_per_game, sizeof m->cards_per_game, jstr(set, NULL, "per_game", ""));
                str_copy(m->cards_shared, sizeof m->cards_shared, jstr(set, NULL, "shared", ""));
                /* A core's per-game cards sync only while its option says so. */
                if (m->cards_option[0] && !m->req_key[0]) {
                    str_copy(m->req_key, sizeof m->req_key, m->cards_option);
                    str_copy(m->req_value, sizeof m->req_value, m->cards_per_game);
                    str_copy(m->req_default, sizeof m->req_default, jstr(set, NULL, "default", ""));
                    if (m->cards_change[0])
                        snprintf(m->req_hint, sizeof m->req_hint, "The memory cards are shared by every game. %s to give each game its own card, which then syncs.", m->cards_change);
                }
                cards_mode_now(m);
            }
            const cJSON *req = cJSON_GetObjectItemCaseSensitive(e, "sync_requires_option");
            if (cJSON_IsObject(req)) {
                str_copy(m->req_key, sizeof m->req_key, jstr(req, NULL, "key", ""));
                str_copy(m->req_value, sizeof m->req_value, jstr(req, NULL, "value", ""));
                str_copy(m->req_default, sizeof m->req_default, jstr(req, NULL, "default", ""));
                str_copy(m->req_hint, sizeof m->req_hint, jstr(req, NULL, "hint", ""));
            }
            /* Platform-wide answer, for the UI; games can override it. */
            check_requirement(m, NULL, NULL, &m->sync_saves, m->sync_note, sizeof m->sync_note);
            if (!strcmp(m->save_layout, "switch")) switch_user_for(m, p, e, vars);
            if (!strcmp(m->save_layout, "x360")) x360_user_for(m, p, e, vars);
            if (m->sync_off) str_copy(m->sync_note, sizeof m->sync_note, "Save sync is off for this platform.");
            cnt++;
        }
        cJSON_Delete(plats);
    }
    /* An extra shares the main emulator's save only when both keep it in the
     * same known format (a PS1 card's "save_format", or one save layout: PS2
     * cards, Switch, PSP... folders); otherwise it syncs under a slot of its
     * own. GameCube's layout shares through its own mount (mount_main_saves). */
    for (int i = 0; i < cnt; i++) {
        if (!maps[i].extra) continue;
        maps[i].own_slot = 1;
        /* The same system (RomM's first slug: Porpoise calls GameCube's
         * folder "gamecube", RetroArch "ngc") and the same format. */
        char sys_i[64];
        str_copy(sys_i, sizeof sys_i, maps[i].romm_slugs);
        sys_i[strcspn(sys_i, ",")] = 0;
        for (int k = 0; k < cnt; k++) {
            char sys_k[64];
            str_copy(sys_k, sizeof sys_k, maps[k].romm_slugs);
            sys_k[strcspn(sys_k, ",")] = 0;
            const int format = maps[i].save_format[0] && !strcmp(profiles_save_family(maps[k].save_format), profiles_save_family(maps[i].save_format));
            const int layout = maps[i].save_layout[0] && strcmp(maps[i].save_layout, "gci") != 0 &&
                               strcmp(maps[i].save_layout, "wii") != 0 && !strcmp(maps[k].save_layout, maps[i].save_layout);
            if (!maps[k].extra && !strcmp(sys_k, sys_i) && (format || layout)) maps[i].own_slot = 0;
        }
    }
    for (int i = 0; i < cnt; i++) mount_main_games(maps, cnt, &maps[i]);
    /* A core twin plays its entry's games, wherever the mount put them. */
    for (int i = 0; i < cnt; i++) {
        if (!maps[i].twin) continue;
        for (int k = 0; k < cnt; k++)
            if (!maps[k].twin && !strcmp(maps[k].profile_id, maps[i].profile_id) && !strcmp(maps[k].romm_slugs, maps[i].romm_slugs)) {
                str_copy(maps[i].rom_dir, sizeof maps[i].rom_dir, maps[k].rom_dir);
                str_copy(maps[i].own_rom_dir, sizeof maps[i].own_rom_dir, maps[k].own_rom_dir);
                break;
            }
    }
    card_methods(maps, cnt);
    *out = maps;
    return cnt;
}

cJSON *profile_firmware_rules(const cJSON *profiles, const profile_map *m) {
    char sys[64];
    str_copy(sys, sizeof sys, m->romm_slugs);
    sys[strcspn(sys, ",")] = 0;
    const cJSON *p, *e;
    cJSON_ArrayForEach(p, profiles) {
        if (strcmp(jstr(p, NULL, "id", "custom"), m->profile_id) != 0) continue;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
            if (strcmp(profiles_system_key(e), sys) != 0) continue;
            const cJSON *rules = state_setting(p, e, "firmware");
            if (cJSON_IsObject(rules)) return cJSON_Duplicate(rules, 1);
            /* Folders the user picked, with no preset: BIOS files only where
             * a BIOS folder was picked too, not guessed below the games. */
            if (!strncmp(m->profile_id, "custom", 6) && !jstr(p, e, "bios_dir", NULL)) return cJSON_Parse("{\"skip\":true}");
            return NULL;
        }
    }
    return NULL;
}

int profile_map_handles(const profile_map *m, const char *slug, const char *fs_slug) {
    char buf[256];
    str_copy(buf, sizeof buf, m->romm_slugs);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if ((slug && str_ieq(t, slug)) || (fs_slug && str_ieq(t, fs_slug))) return 1;
    }
    return 0;
}

const profile_map *profile_find_for_platform(const profile_map *maps, int n, const char *slug,
                                             const char *fs_slug) {
    /* The main emulator first; an extra only when the system has no main one. */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++)
            if (maps[i].extra == pass && !maps[i].twin && profile_map_handles(&maps[i], slug, fs_slug)) return &maps[i];
    return NULL;
}
