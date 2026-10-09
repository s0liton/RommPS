#include "profiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

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
    "{\"romm\":[\"genesis-slash-megadrive\",\"genesis\",\"megadrive\"],\"dir\":\"megadrive\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"sms\",\"mastersystem\"],\"dir\":\"mastersystem\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"gamegear\",\"gg\"],\"dir\":\"gamegear\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"segacd\",\"sega-cd\"],\"dir\":\"segacd\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"sg1000\"],\"dir\":\"sg1000\",\"emulator\":\"genesis_plus_gx\",\"core_name\":\"Genesis Plus GX\"},"
    "{\"romm\":[\"arcade\",\"fbneo\",\"neogeoaes\",\"neogeomvs\",\"cps1\",\"cps2\",\"cps3\"],\"dir\":\"fbneo\",\"emulator\":\"fbneo\",\"core_name\":\"FinalBurn Neo\"},"
    /* Saves made of several files, synced as one zip per game. */
    "{\"romm\":[\"ps2\"],\"dir\":\"ps2\",\"emulator\":\"pcsx2\",\"core_name\":\"LRPS2\",\"save_exts\":\".ps2\","
    "\"sync_requires_option\":{\"key\":\"pcsx2_shared_memory_cards\",\"value\":\"disabled\",\"default\":\"enabled\","
    "\"hint\":\"LRPS2 uses one shared memory card for all games. Turn off Shared Memory Cards in the core options to give each game its own card, which then syncs.\"}},"
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
    "{\"romm\":[\"genesis-slash-megadrive\",\"genesis\",\"megadrive\",\"sms\",\"gamegear\"],\"emulator\":\"mednafen_md\"},"
    "{\"romm\":[\"tg16\",\"turbografx16--1\",\"pce\",\"pcengine\"],\"emulator\":\"mednafen_pce\"},"
    "{\"romm\":[\"ngp\",\"ngpc\",\"neo-geo-pocket\",\"neo-geo-pocket-color\"],\"emulator\":\"mednafen_ngp\"},"
    "{\"romm\":[\"wonderswan\",\"wonderswan-color\",\"ws\",\"wsc\"],\"emulator\":\"mednafen_wswan\"},"
    "{\"romm\":[\"virtualboy\",\"vb\"],\"emulator\":\"mednafen_vb\"},"
    "{\"romm\":[\"lynx\"],\"emulator\":\"mednafen_lynx\"}"
    "]},"
    "{\"id\":\"ps2-classics\",\"name\":\"PS2 ISOs (for PS2 FPKG tools)\",\"enabled\":false,"
    "\"root\":\"/mnt/usb0\",\"rom_dir\":\"{root}/ps2\",\"bios_dir\":\"{root}/ps2/bios\","
    "\"sync_saves\":false,"
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
        if (!strncmp(line, key, kl) && (line[kl] == ' ' || line[kl] == '=')) {
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
static int core_option(const profile_map *m, const char *game, const char *cdir, char *out, size_t n) {
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
        int found = txt && ra_cfg_get(txt, m->req_key, out, n);
        free(txt);
        if (found) return 1;
    }
    return 0;
}

static void check_requirement(const profile_map *m, const char *game, const char *cdir, int *sync, char *note,
                              size_t nn) {
    *sync = m->base_sync_saves;
    note[0] = 0;
    if (!m->req_key[0] || !m->base_sync_saves) return;
    char val[64];
    if (!m->ra_cfg_dir[0] || !core_option(m, game, cdir, val, sizeof val)) str_copy(val, sizeof val, m->req_default);
    if (str_ieq(val, m->req_value)) return;
    *sync = 0;
    if (m->req_hint[0])
        str_copy(note, nn, m->req_hint);
    else
        snprintf(note, nn, "saves sync when core option %s is \"%s\" (now \"%s\")", m->req_key, m->req_value, val);
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

/* Whether a profile's list ("exclude", "extra") names a system. */
static int profile_lists(const cJSON *profile, const char *list, const char *key) {
    const cJSON *x;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(profile, list))
        if (cJSON_IsString(x) && str_ieq(x->valuestring, key)) return 1;
    return 0;
}

int profiles_excludes(const cJSON *profile, const char *key) { return profile_lists(profile, "exclude", key); }

int profiles_expand(const cJSON *profiles, profile_map **out) {
    int cap = 0, cnt = 0;
    profile_map *maps = NULL;
    const cJSON *p;
    cJSON_ArrayForEach(p, profiles) {
        if (!jbool(p, NULL, "enabled", 1)) continue;
        char root[PATH_MAX_LEN];
        str_copy(root, sizeof root, jstr(p, NULL, "root", ""));

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

        const cJSON *plats = cJSON_GetObjectItemCaseSensitive(p, "platforms");
        const cJSON *e;
        cJSON_ArrayForEach(e, plats) {
            if (profiles_excludes(p, profiles_system_key(e))) continue; /* another emulator plays it */
            if (cnt == cap) {
                cap = cap ? cap * 2 : 16;
                maps = realloc(maps, sizeof *maps * (size_t)cap);
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

            const char *vars[] = {"root", root, "platform", m->platform_dir, "data",
                                  plat_data_dir(), "rom_dir", NULL, NULL};
            str_template(m->rom_dir, sizeof m->rom_dir, jstr(p, e, "rom_dir", "{root}/roms/{platform}"), vars);
            vars[7] = m->rom_dir;
            str_template(m->save_dir, sizeof m->save_dir, jstr(p, e, "save_dir", "{rom_dir}"), vars);
            str_template(m->state_dir, sizeof m->state_dir, jstr(p, e, "state_dir", "{rom_dir}"), vars);
            str_template(m->bios_dir, sizeof m->bios_dir, jstr(p, e, "bios_dir", "{root}/bios"), vars);
            /* The emulator's config wins unless the platform entry sets a folder. */
            if (ra_save[0] && !cJSON_GetObjectItemCaseSensitive(e, "save_dir"))
                str_copy(m->save_dir, sizeof m->save_dir, ra_save);
            if (ra_state[0] && !cJSON_GetObjectItemCaseSensitive(e, "state_dir"))
                str_copy(m->state_dir, sizeof m->state_dir, ra_state);
            if (ra_sys[0] && !cJSON_GetObjectItemCaseSensitive(e, "bios_dir"))
                str_copy(m->bios_dir, sizeof m->bios_dir, ra_sys);
            for (int k = 0; k < 2; k++) {
                m->explicit_dir[k] = cJSON_GetObjectItemCaseSensitive(e, k ? "state_dir" : "save_dir") != NULL;
                if (m->explicit_dir[k]) continue;
                m->in_content[k] = ra_in_content[k];
                m->by_content[k] = ra_by_content[k];
                m->by_core[k] = ra_by_core[k];
            }
            str_copy(m->core_name, sizeof m->core_name, jstr(p, e, "core_name", ""));
            str_copy(m->save_layout, sizeof m->save_layout, jstr(p, e, "save_layout", ""));
            str_copy(m->bundle_dirs, sizeof m->bundle_dirs, jstr(p, e, "bundle_dirs", ""));

            str_copy(m->save_exts, sizeof m->save_exts, jstr(p, e, "save_exts", ".srm,.sav"));
            str_copy(m->state_exts, sizeof m->state_exts, jstr(p, e, "state_exts", ".state*"));
            str_copy(m->save_name, sizeof m->save_name, jstr(p, e, "save_name", "{rom_stem}{ext}"));
            str_copy(m->save_stems, sizeof m->save_stems, jstr(p, e, "save_stems", ""));
            str_copy(m->state_stems, sizeof m->state_stems, jstr(p, e, "state_stems", ""));
            str_copy(m->state_name, sizeof m->state_name, jstr(p, e, "state_name", "{rom_stem}{ext}"));
            str_copy(m->save_format, sizeof m->save_format, jstr(p, e, "save_format", ""));
            str_template(m->memcards, sizeof m->memcards, jstr(p, e, "memcards", ""), vars);
            m->extra = profile_lists(p, "extra", profiles_system_key(e));
            m->sync_saves = m->base_sync_saves = jbool(p, e, "sync_saves", 1);
            str_copy(m->root, sizeof m->root, root);
            str_copy(m->ra_cfg_dir, sizeof m->ra_cfg_dir, ra_cfg_dir);
            const cJSON *req = cJSON_GetObjectItemCaseSensitive(e, "sync_requires_option");
            if (cJSON_IsObject(req)) {
                str_copy(m->req_key, sizeof m->req_key, jstr(req, NULL, "key", ""));
                str_copy(m->req_value, sizeof m->req_value, jstr(req, NULL, "value", ""));
                str_copy(m->req_default, sizeof m->req_default, jstr(req, NULL, "default", ""));
                str_copy(m->req_hint, sizeof m->req_hint, jstr(req, NULL, "hint", ""));
            }
            /* Platform-wide answer, for the UI; games can override it. */
            check_requirement(m, NULL, NULL, &m->sync_saves, m->sync_note, sizeof m->sync_note);
            cnt++;
        }
    }
    /* An extra shares the main emulator's save only when both keep it in the
     * same known format; otherwise it syncs under a slot of its own. */
    for (int i = 0; i < cnt; i++) {
        if (!maps[i].extra) continue;
        maps[i].own_slot = 1;
        for (int k = 0; k < cnt; k++)
            if (!maps[k].extra && !strcmp(maps[k].platform_dir, maps[i].platform_dir) && maps[i].save_format[0] &&
                !strcmp(maps[k].save_format, maps[i].save_format))
                maps[i].own_slot = 0;
    }
    *out = maps;
    return cnt;
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
            if (maps[i].extra == pass && profile_map_handles(&maps[i], slug, fs_slug)) return &maps[i];
    return NULL;
}
