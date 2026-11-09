#include "cfgwrite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cfgvars.h"
#include "config.h"
#include "detect.h"
#include "platform.h"
#include "presets.h"
#include "profiles.h"
#include "state.h"
#include "util.h"

#define PASS_SEC 5

/* A catalog entry's "config_writes", on a platform of its profile or on the
 * profile itself, each one of:
 *   {"file": ..., "section": "UI", "array": "Paths\\gamedirs", "match": "path",
 *    "seed": [{...}, ...], "add": {"path": "{rom_dir}", "deep_scan": "false"}}
 *       a row in a Qt array (cfgvars_array_add); seed's rows first when the
 *       array is missing or empty.
 *   {"file": ..., "section": "GameList", "key": "RecursivePaths", "add": "{rom_dir}"}
 *       one more line of a key kept as a list (cfgvars_list_add); with
 *       "format": "json", one more string in a top-level array
 *       (cfgvars_json_array_add: Ryujinx's "autoload_dirs").
 *   {"file": ..., "section": "", "key": "LastROMFolder", "set": "{rom_dir}",
 *    "quote": true, "sep": " = ", "if_unset": true}
 *       one key's value (cfgvars_set).
 * Values may use {rom_dir}, {update_dir}, {dlc_dir}, {bios}, {root} and
 * {save_dir}. The file has to be there: "create": true makes it, otherwise
 * it waits for the emulator's first start (Eden asks to bring in another
 * emulator's data only while it has no config folder). "when" is "closed",
 * the only time: nothing is written while one of the entry's title_ids runs. */
static const cJSON *writes_for(const profile_map *m) {
    const cJSON *prof = cJSON_GetObjectItemCaseSensitive(detect_catalog_entry(m->profile_id), "profile"), *pe;
    char sys[64];
    str_copy(sys, sizeof sys, m->romm_slugs);
    sys[strcspn(sys, ",")] = 0;
    cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(prof, "platforms")) {
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(pe, "config_writes");
        if (!strcmp(profiles_system_key(pe), sys) && cJSON_IsArray(w)) return w;
    }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(prof, "config_writes");
    return cJSON_IsArray(w) ? w : NULL;
}

static int running(const profile_map *m) {
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(detect_catalog_entry(m->profile_id), "title_ids"))
        if (cJSON_IsString(t) && plat_app_alive(t->valuestring) == 1) return 1;
    return 0;
}

/* tpl with the profile's folders filled in; 0 if one isn't known. */
static int filled(const profile_map *m, const char *tpl, char *out, size_t n) {
    const char *vars[] = {"rom_dir", m->rom_dir,  "update_dir", m->update_dir, "dlc_dir", m->dlc_dir, "bios",
                          m->bios_dir, "root", m->root, "save_dir", m->save_dir, NULL};
    str_template(out, n, tpl ? tpl : "", vars);
    return out[0] && !strchr(out, '{');
}

/* An object's string values as a row, filled in. The strings live in store. */
#define ROW_MAX 8
typedef struct {
    cfgvars_kv kv[ROW_MAX];
    char val[ROW_MAX][PATH_MAX_LEN];
    cfgvars_row row;
} row_buf;

static int make_row(const profile_map *m, const cJSON *o, row_buf *b) {
    const cJSON *x;
    b->row.kv = b->kv;
    b->row.n = 0;
    cJSON_ArrayForEach(x, o) {
        if (!cJSON_IsString(x) || b->row.n == ROW_MAX || !filled(m, x->valuestring, b->val[b->row.n], sizeof b->val[0])) return 0;
        b->kv[b->row.n].key = x->string;
        b->kv[b->row.n].value = b->val[b->row.n];
        b->row.n++;
    }
    return b->row.n > 0;
}

/* Said once a run: a config that isn't there yet. */
static void say_waiting(const profile_map *m, const char *file) {
    static char said[16][PATH_MAX_LEN];
    static int nsaid;
    for (int i = 0; i < nsaid; i++)
        if (!strcmp(said[i], file)) return;
    if (nsaid < 16) str_copy(said[nsaid++], sizeof said[0], file);
    LOGI("%s: no %s yet; its folders go in once %s has started once", m->profile_name, file, m->profile_name);
}

static int apply_one(const profile_map *m, const cJSON *w) {
    char file[PATH_MAX_LEN];
    if (!filled(m, jget_str(w, "file", ""), file, sizeof file)) return 0;
    const char *section = jget_str(w, "section", ""), *key = jget_str(w, "key", ""), *array = jget_str(w, "array", "");
    char *txt = read_file(file, NULL);
    if (!txt && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "create"))) {
        say_waiting(m, file);
        return 0;
    }
    if (!txt) txt = calloc(1, 1);
    char *out = NULL, what[PATH_MAX_LEN + 256] = "";
    const cJSON *add = cJSON_GetObjectItemCaseSensitive(w, "add");
    if (array[0] && cJSON_IsObject(add)) {
        row_buf *rows = calloc(ROW_MAX + 1, sizeof *rows);
        cfgvars_row seed[ROW_MAX];
        int nseed = 0, ok = rows != NULL;
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(w, "seed")) {
            if (!ok || nseed == ROW_MAX) break;
            ok = make_row(m, s, &rows[nseed]);
            seed[nseed] = rows[nseed].row;
            nseed++;
        }
        if (ok && make_row(m, add, &rows[ROW_MAX])) {
            out = cfgvars_array_add(txt, section, array, jget_str(w, "match", "path"), seed, nseed, &rows[ROW_MAX].row);
            const char *v = NULL;
            for (int k = 0; k < rows[ROW_MAX].row.n; k++) v = v ? v : rows[ROW_MAX].kv[k].value;
            snprintf(what, sizeof what, "%s in %s", v ? v : "", array);
        }
        free(rows);
    } else if (key[0] && cJSON_IsString(add)) {
        char v[PATH_MAX_LEN];
        const int json = !strcmp(jget_str(w, "format", "ini"), "json");
        if (filled(m, add->valuestring, v, sizeof v))
            out = json ? cfgvars_json_array_add(txt, key, v) : cfgvars_list_add(txt, section, key, v);
        snprintf(what, sizeof what, "%s in %s", v, key);
    } else if (key[0] && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(w, "set"))) {
        char v[PATH_MAX_LEN];
        const cJSON *sep = cJSON_GetObjectItemCaseSensitive(w, "sep");
        if (filled(m, jget_str(w, "set", ""), v, sizeof v))
            out = cfgvars_set(txt, section, key, v, cJSON_IsString(sep) ? sep->valuestring : NULL,
                              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "quote")),
                              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "if_unset")));
        snprintf(what, sizeof what, "%s as %s", v, key);
    }
    free(txt);
    if (!out) return 0;
    const int ok = mkdir_parent(file) == 0 && write_file_atomic(file, out, strlen(out)) == 0;
    free(out);
    if (ok) LOGI("%s: put %s (%s), so it sees the games and files that come down", m->profile_name, what, file);
    else LOGW("%s: could not write %s", m->profile_name, file);
    return ok;
}

/* Whether any emulator in the catalog has config_writes (none on the consoles). */
static int any_writes(void) {
    const cJSON *e, *pe;
    cJSON_ArrayForEach(e, presets_catalog()) {
        const cJSON *prof = cJSON_GetObjectItemCaseSensitive(e, "profile");
        if (cJSON_GetObjectItemCaseSensitive(prof, "config_writes")) return 1;
        cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(prof, "platforms"))
            if (cJSON_GetObjectItemCaseSensitive(pe, "config_writes")) return 1;
    }
    return 0;
}

int cfgwrite_now(void) {
    if (!any_writes()) return 0;
    profile_map *maps = NULL;
    config_lock();
    const int ready = g_cfg.setup_complete;
    const int n = ready ? profiles_expand(g_cfg.profiles, &maps) : 0;
    config_unlock();
    int changed = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *ws = writes_for(&maps[i]), *w;
        if (!ws || running(&maps[i])) continue;
        cJSON_ArrayForEach(w, ws) changed += apply_one(&maps[i], w);
    }
    profiles_free(maps);
    return changed;
}

static void *cfgwrite_thread(void *arg) {
    (void)arg;
    for (;;) {
        cfgwrite_now();
        sleep(PASS_SEC);
    }
    return NULL;
}

void cfgwrite_init(void) { thread_start(cfgwrite_thread, NULL); }
