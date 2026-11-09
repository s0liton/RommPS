#include "presets.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#include "cfgvars.h"
#include "platform.h"
#include "util.h"
#include "config.h"
#include "detect.h"
#include "http.h"
#include "monocypher-ed25519.h"
#include "update_keys.h"
#include "state.h"

#ifndef PRESETS_URL
#define PRESETS_URL "https://raw.githubusercontent.com/s0liton/RommPS/presets"
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static cJSON *g_file;
/* g_file as read, before its placeholders were filled: kept to fill them
 * again when the machine's folders change (presets_refresh). */
static cJSON *g_raw;

static void seen_reset(void);

static cJSON *resolved(const cJSON *raw) {
    cJSON *f = cJSON_Duplicate(raw, 1);
    seen_reset();
    presets_resolve(f);
    return f;
}

static char g_source[16] = "built-in";

static int g_checking;
static char g_error[256];
static time_t g_checked_at;



int presets_version_cmp(const char *a, const char *b) {
    int x[4] = {0}, y[4] = {0};
    sscanf(a, "%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3]);
    sscanf(b, "%d.%d.%d.%d", &y[0], &y[1], &y[2], &y[3]);
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static void download_path(const char *name, char *out, size_t n) {
    char dir[PATH_MAX_LEN];
    path_join(dir, sizeof dir, plat_data_dir(), "presets");
    path_join(out, n, dir, name);
}

/* Where placeholders come from: an entry's own "vars", the entries around
 * it, then plat_info's catalog_var; and, while a var's file is read, the
 * folder it's in ({file_dir}). */
typedef struct resolver {
    const struct resolver *up;
    const cJSON *vars;
    const char *file_dir;
    const char *flatpak; /* the entry's Flatpak, when it's installed as one: "" if not */
} resolver;

static int lookup(const resolver *r, const char *name, char *out, size_t n) {
    for (const resolver *x = r; x; x = x->up) {
        if (x->file_dir && !strcmp(name, "file_dir")) {
            str_copy(out, n, x->file_dir);
            return 1;
        }
        const cJSON *v = x->vars ? cJSON_GetObjectItemCaseSensitive(x->vars, name) : NULL;
        if (cJSON_IsString(v)) {
            str_copy(out, n, v->valuestring);
            return 1;
        }
    }
    int (*var)(const char *, char *, size_t) = plat_info()->catalog_var;
    return var && var(name, out, n);
}

/* in with the placeholders r knows filled in; *left counts the ones it
 * doesn't ({root}, {rom_dir}: profiles fill those). 0 if too long. */
static int fill(const char *in, const resolver *r, char *out, size_t n, int *left) {
    char name[64], value[PATH_MAX_LEN];
    size_t o = 0;
    if (left) *left = 0;
    while (*in && o < n - 1) {
        const char *end = *in == '{' ? strchr(in, '}') : NULL;
        if (end && (size_t)(end - in - 1) < sizeof name) {
            snprintf(name, sizeof name, "%.*s", (int)(end - in - 1), in + 1);
            if (lookup(r, name, value, sizeof value)) {
                o += (size_t)snprintf(out + o, n - o, "%s", value);
                if (o >= n) return 0;
                in = end + 1;
                continue;
            }
            if (left) (*left)++;
        }
        out[o++] = *in++;
    }
    out[o] = 0;
    return !*in;
}

/* One string's placeholders, when they're known. */
static void resolve_string(cJSON *s, const resolver *r) {
    if (!strchr(s->valuestring, '{')) return;
    char out[PATH_MAX_LEN * 2];
    if (fill(s->valuestring, r, out, sizeof out, NULL) && strcmp(out, s->valuestring) != 0) cJSON_SetValuestring(s, out);
}

/* The files and folders vars looked at, and how they were: when one
 * changes (an emulator installed, its config saved), the catalog is filled
 * in again (presets_vars_changed). */
typedef struct {
    char *path;
    time_t mtime;
    int exists;
} seen_file;
static seen_file *g_seen;
static int g_nseen, g_seen_cap;
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;

static void seen_reset(void) {
    pthread_mutex_lock(&g_seen_lock);
    for (int i = 0; i < g_nseen; i++) free(g_seen[i].path);
    g_nseen = 0;
    pthread_mutex_unlock(&g_seen_lock);
}

static int seen(const char *path) {
    struct stat st;
    const int exists = stat(path, &st) == 0;
    pthread_mutex_lock(&g_seen_lock);
    int i = 0;
    while (i < g_nseen && strcmp(g_seen[i].path, path) != 0) i++;
    if (i == g_nseen && g_nseen == g_seen_cap) {
        seen_file *grown = realloc(g_seen, (size_t)(g_seen_cap + 64) * sizeof *g_seen);
        if (grown) g_seen = grown, g_seen_cap += 64;
    }
    if (i == g_nseen && g_nseen < g_seen_cap && (g_seen[i].path = strdup(path))) {
        g_seen[i].exists = exists;
        g_seen[i].mtime = exists ? st.st_mtime : 0;
        g_nseen++;
    }
    pthread_mutex_unlock(&g_seen_lock);
    return exists;
}

int presets_vars_changed(void) {
    int changed = 0;
    pthread_mutex_lock(&g_seen_lock);
    for (int i = 0; i < g_nseen && !changed; i++) {
        struct stat st;
        const int exists = stat(g_seen[i].path, &st) == 0;
        changed = exists != g_seen[i].exists || (exists && st.st_mtime != g_seen[i].mtime);
    }
    pthread_mutex_unlock(&g_seen_lock);
    return changed;
}

/* "~", $HOME and ${HOME} as the home folder (MAME's ini says $HOME/.mame). */
static void expand_home(char *s, size_t n, const resolver *r) {
    char home[PATH_MAX_LEN], out[PATH_MAX_LEN];
    if (!lookup(r, "home", home, sizeof home)) str_copy(home, sizeof home, getenv("HOME") ? getenv("HOME") : "");
    size_t o = 0;
    const char *in = s;
    if (in[0] == '~' && (in[1] == '/' || !in[1])) {
        o = (size_t)snprintf(out, sizeof out, "%s", home);
        in++;
    }
    while (*in && o < sizeof out - 1) {
        size_t hl = !strncmp(in, "${HOME}", 7) ? 7 : !strncmp(in, "$HOME", 5) && !isalnum((unsigned char)in[5]) && in[5] != '_' ? 5 : 0;
        if (!hl) {
            out[o++] = *in++;
            continue;
        }
        o += (size_t)snprintf(out + o, sizeof out - o, "%s", home);
        in += hl;
    }
    if (o >= sizeof out) return;
    out[o] = 0;
    str_copy(s, n, out);
}

/* A Flatpak's sandbox and what it may see of the machine: the filesystems=
 * of its [Context], as installed (its metadata) and as overridden since
 * (flatpak override, EmuDeck's --filesystem=host among them), user ones last.
 * "!x" takes x away. Into list, ';' separated. 1 if it's installed. */
static int flatpak_context(const resolver *r, const char *id, char *list, size_t n) {
    char home[PATH_MAX_LEN], sys[PATH_MAX_LEN], p[PATH_MAX_LEN], v[2048];
    if (!lookup(r, "home", home, sizeof home) || !lookup(r, "fpsys", sys, sizeof sys)) return 0;
    char user[PATH_MAX_LEN];
    snprintf(user, sizeof user, "%s/.local/share/flatpak", home);
    const char *roots[2] = {sys, user};
    int installed = 0;
    list[0] = 0;
    for (int i = 0; i < 2 && !installed; i++) {
        snprintf(p, sizeof p, "%s/app/%s/current/active/metadata", roots[i], id);
        char *txt = seen(p) ? read_file(p, NULL) : NULL;
        if (!txt) continue;
        installed = 1;
        if (cfgvars_lookup(txt, "ini", "Context", "filesystems", v, sizeof v)) str_copy(list, n, v);
        free(txt);
    }
    if (!installed) return 0;
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++) {
            snprintf(p, sizeof p, "%s/overrides/%s", roots[i], k ? id : "global");
            char *txt = seen(p) ? read_file(p, NULL) : NULL;
            if (txt && cfgvars_lookup(txt, "ini", "Context", "filesystems", v, sizeof v)) {
                size_t l = strlen(list);
                snprintf(list + l, n - l, "%s%s", l ? ";" : "", v);
            }
            free(txt);
        }
    return 1;
}

/* Whether a Flatpak with these filesystems= sees path. Its own ~/.var/app
 * folder it always does; host is everything a user has, home their home
 * folder; anything else, the folder it names ("~/Games", "/run/media"). */
static int flatpak_sees(const char *list, const char *id, const char *home, const char *path) {
    char own[PATH_MAX_LEN], item[PATH_MAX_LEN], base[PATH_MAX_LEN];
    snprintf(own, sizeof own, "%s/.var/app/%s", home, id);
    size_t ol = strlen(own), hl = strlen(home);
    if (!strncmp(path, own, ol) && (path[ol] == '/' || !path[ol])) return 1;
    const int in_home = !strncmp(path, home, hl) && (path[hl] == '/' || !path[hl]);
    int sees = 0;
    for (const char *it = list; *it;) {
        size_t il = strcspn(it, ";");
        snprintf(item, sizeof item, "%.*s", (int)il, it);
        it += il + (it[il] == ';');
        const int negated = item[0] == '!';
        char *name = item + negated, *colon = strchr(name, ':');
        if (colon) *colon = 0; /* :ro, :rw, :create: reading is all that's asked */
        int covers = 0;
        if (!strcmp(name, "host")) covers = 1;
        else if (!strcmp(name, "home")) covers = in_home;
        else {
            if (name[0] == '~' || !strncmp(name, "home/", 5)) snprintf(base, sizeof base, "%s/%s", home, name + (name[0] == '~' ? 2 : 5));
            else if (name[0] == '/') str_copy(base, sizeof base, name);
            else continue; /* xdg-*: not where games are kept */
            size_t bl = strlen(base);
            while (bl > 1 && base[bl - 1] == '/') base[--bl] = 0;
            covers = !strncmp(path, base, bl) && (path[bl] == '/' || !path[bl]);
        }
        if (covers) sees = !negated;
    }
    return sees;
}

static void replace_all(char *s, size_t n, const char *what, const char *with) {
    char out[PATH_MAX_LEN];
    size_t o = 0, wl = strlen(what);
    if (!wl) return;
    for (const char *p = s; *p && o < sizeof out - 1;) {
        if (!strncmp(p, what, wl)) {
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", with);
            p += wl;
        } else {
            out[o++] = *p++;
        }
    }
    if (o >= sizeof out) return;
    out[o] = 0;
    str_copy(s, n, out);
}

/* One of an entry's "vars": a folder or a setting out of the emulator's own
 * config, so its paths follow the install that's there (a Flatpak's or an
 * AppImage's folders, and wherever the user or EmuDeck pointed them):
 *   {"path": [folders or files], "default": "..."}
 *       the first that exists, else default, else the first.
 *   {"file": [config files], "format": "ini|toml|yaml|xml|space",
 *    "section": "...", "key": "...", "default": "...", "base": "...",
 *    "subst": {"$(EmulatorDir)": "..."}, "split": ";", "ignore": "/app/", "dirname": true,
 *    "then": "{value}/Super Famicom", "exists": true, "plain": true}
 *       key's value in the first file that exists (cfgvars_lookup); unset,
 *       empty or starting with ignore (a folder inside a Flatpak's sandbox):
 *       default; no default, no var. A value's text in subst is replaced,
 *       split keeps what's before the first of its characters (a list), "~"
 *       and $HOME are the home folder, a relative one is under base (default:
 *       {file_dir}, the file's folder), and dirname keeps the folder of what
 *       the file said (a BIOS file's: not of the default). then, when the file
 *       had it, makes the var from it ({value}: ares keeps each system's saves
 *       in a folder of its own under the one it's set to). exists: a value
 *       naming a file or folder that isn't there counts as unset (EmuDeck's
 *       Cemu template names a game folder of another machine's). plain: a
 *       setting, not a path (Vita3K's user-id "00"): taken as it is.
 *   {"readable": [folders]}
 *       the first the entry's Flatpak can read ("{roms}", else "{sandbox}/roms"
 *       in its own folder), going by its permissions and overrides; the
 *       first when it isn't installed as a Flatpak (an AppImage sees them all).
 * Strings in it may use the platform's placeholders and earlier vars. */
static int eval_var(const cJSON *spec, const resolver *r, char *out, size_t n) {
    const cJSON *readable = cJSON_GetObjectItemCaseSensitive(spec, "readable"), *c;
    if (readable) {
        const char *id = "";
        for (const resolver *x = r; x && !id[0]; x = x->up) id = x->flatpak ? x->flatpak : "";
        char list[4096] = "", home[PATH_MAX_LEN] = "", cand[PATH_MAX_LEN], first[PATH_MAX_LEN] = "";
        const int sandboxed = id[0] && flatpak_context(r, id, list, sizeof list) && lookup(r, "home", home, sizeof home);
        int left;
        for (c = cJSON_IsArray(readable) ? readable->child : readable; c; c = cJSON_IsArray(readable) ? c->next : NULL) {
            if (!cJSON_IsString(c) || !fill(c->valuestring, r, cand, sizeof cand, &left) || left) continue;
            expand_home(cand, sizeof cand, r);
            cfgvars_normalize(cand);
            if (!first[0]) str_copy(first, sizeof first, cand);
            if (!sandboxed || flatpak_sees(list, id, home, cand)) {
                str_copy(out, n, cand);
                return 1;
            }
        }
        if (!first[0]) return 0;
        str_copy(out, n, first); /* it sees none of them: the first, as configured */
        return 1;
    }
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(spec, "path");
    const int is_path = path != NULL;
    const cJSON *cands = is_path ? path : cJSON_GetObjectItemCaseSensitive(spec, "file");
    char cand[PATH_MAX_LEN], chosen[PATH_MAX_LEN] = "", val[PATH_MAX_LEN] = "", tmp[PATH_MAX_LEN];
    int found = 0, left;
    for (c = cJSON_IsArray(cands) ? cands->child : cands; c && !found; c = cJSON_IsArray(cands) ? c->next : NULL) {
        if (!cJSON_IsString(c) || !fill(c->valuestring, r, cand, sizeof cand, &left) || left) continue;
        expand_home(cand, sizeof cand, r);
        if (!chosen[0]) str_copy(chosen, sizeof chosen, cand);
        if (seen(cand)) {
            str_copy(chosen, sizeof chosen, cand);
            found = 1;
        }
    }
    char dir[PATH_MAX_LEN];
    str_copy(dir, sizeof dir, chosen);
    if (!is_path) {
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
    }
    const resolver inner = {r, NULL, dir, NULL};
    int have = 0;
    if (is_path) {
        have = found;
        if (found) str_copy(val, sizeof val, chosen);
    } else if (found) {
        char *txt = read_file(chosen, NULL);
        have = txt && cfgvars_lookup(txt, jget_str(spec, "format", "ini"), jget_str(spec, "section", ""),
                                     jget_str(spec, "key", ""), val, sizeof val) && val[0];
        free(txt);
        const char *split = jget_str(spec, "split", ""), *ignore = jget_str(spec, "ignore", "");
        if (have && split[0]) val[strcspn(val, split)] = 0;
        have = have && val[0] && !(ignore[0] && !strncmp(val, ignore, strlen(ignore)));
        const cJSON *sub = have ? cJSON_GetObjectItemCaseSensitive(spec, "subst") : NULL, *x;
        cJSON_ArrayForEach(x, sub)
            if (cJSON_IsString(x) && fill(x->valuestring, &inner, tmp, sizeof tmp, NULL)) replace_all(val, sizeof val, x->string, tmp);
        const char *then = jget_str(spec, "then", NULL);
        if (have && then) {
            expand_home(val, sizeof val, r);
            str_copy(tmp, sizeof tmp, then);
            replace_all(tmp, sizeof tmp, "{value}", val);
            if (!fill(tmp, &inner, val, sizeof val, NULL)) return 0;
        }
    }
    if (have && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(spec, "exists"))) {
        str_copy(tmp, sizeof tmp, val);
        expand_home(tmp, sizeof tmp, r);
        if (tmp[0] != '/') {
            char rel[PATH_MAX_LEN];
            path_join(rel, sizeof rel, dir, tmp);
            str_copy(tmp, sizeof tmp, rel);
        }
        have = seen(tmp);
    }
    if (!have) {
        const char *def = jget_str(spec, "default", NULL);
        if (def) {
            if (!fill(def, &inner, val, sizeof val, NULL)) return 0;
        } else if (is_path && chosen[0]) {
            str_copy(val, sizeof val, chosen);
        } else {
            return 0;
        }
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(spec, "plain"))) {
        str_copy(out, n, val);
        return val[0] != 0;
    }
    expand_home(val, sizeof val, r);
    if (val[0] && val[0] != '/' && val[0] != '{') {
        char base[PATH_MAX_LEN];
        if (!fill(jget_str(spec, "base", "{file_dir}"), &inner, base, sizeof base, NULL)) return 0;
        path_join(tmp, sizeof tmp, base, val);
        str_copy(val, sizeof val, tmp);
    }
    if (val[0] == '/') cfgvars_normalize(val);
    if (have && !is_path && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(spec, "dirname"))) {
        char *slash = strrchr(val, '/');
        if (slash && slash > val) *slash = 0;
    }
    str_copy(out, n, val);
    return val[0] != 0;
}

static void resolve_node(cJSON *node, const resolver *r) {
    cJSON *c;
    cJSON_ArrayForEach(c, node) {
        if (cJSON_IsString(c)) {
            resolve_string(c, r);
            continue;
        }
        const cJSON *spec = cJSON_IsObject(c) ? cJSON_GetObjectItemCaseSensitive(c, "vars") : NULL;
        if (!cJSON_IsObject(spec)) {
            resolve_node(c, r);
            continue;
        }
        /* An entry with vars: worked out in order, each able to use the ones
         * before it, then its strings filled with them. They stay in the
         * entry, filled in, for diagnostics. An entry naming its Flatpak
         * ("flatpak": id or [ids]) also has {sandbox}: that app's own data
         * folder, the one place it can always reach. */
        cJSON *vals = cJSON_CreateObject();
        char fp[256] = "", val[PATH_MAX_LEN];
        const cJSON *fps = cJSON_GetObjectItemCaseSensitive(c, "flatpak"), *f;
        const char *first_fp = NULL;
        for (f = cJSON_IsArray(fps) ? fps->child : fps; f && !fp[0]; f = cJSON_IsArray(fps) ? f->next : NULL) {
            if (!cJSON_IsString(f)) continue;
            if (!first_fp) first_fp = f->valuestring;
            const resolver probe = {r, NULL, NULL, NULL};
            char ctx[4096];
            if (flatpak_context(&probe, f->valuestring, ctx, sizeof ctx)) str_copy(fp, sizeof fp, f->valuestring);
        }
        const resolver inner = {r, vals, NULL, fp};
        if (first_fp && lookup(r, "home", val, sizeof val)) {
            char sb[PATH_MAX_LEN];
            snprintf(sb, sizeof sb, "%s/.var/app/%s/data", val, fp[0] ? fp : first_fp);
            cJSON_AddStringToObject(vals, "sandbox", sb);
        }
        const cJSON *v;
        cJSON_ArrayForEach(v, spec)
            if (v->string && cJSON_IsObject(v) && eval_var(v, &inner, val, sizeof val)) cJSON_AddStringToObject(vals, v->string, val);
        cJSON_ReplaceItemInObjectCaseSensitive(c, "vars", vals);
        cJSON *x;
        cJSON_ArrayForEach(x, c)
            if (x != vals) {
                if (cJSON_IsString(x)) resolve_string(x, &inner);
                else resolve_node(x, &inner);
            }
    }
}

void presets_resolve(cJSON *node) {
    if (!node) return;
    const resolver top = {NULL, NULL, NULL, NULL};
    resolve_node(node, &top);
}

int presets_fill(const char *entry_id, const char *tpl, char *out, size_t n) {
    const cJSON *e, *vars = NULL;
    cJSON_ArrayForEach(e, presets_catalog())
        if (!strcmp(jget_str(e, "id", ""), entry_id)) vars = cJSON_GetObjectItemCaseSensitive(e, "vars");
    const resolver r = {NULL, vars, NULL, NULL};
    int left = 0;
    return fill(tpl, &r, out, n, &left) && !left;
}

static cJSON *usable(cJSON *f, const char *from) {
    const char *why = !cJSON_IsObject(f) ? "Not valid JSON"
                    : jget_num(f, "format", 0) < 1 || jget_num(f, "format", 0) > PRESETS_FORMAT ? "In a format this version cannot read"
                    : strcmp(jget_str(f, "console", ""), plat_info()->name) != 0 ? "Not for this console"
                    : presets_version_cmp(jget_str(f, "version", ""), "0") <= 0 ? "Missing its version value"
                    : !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(f, "emulators")) ? "Missing emulators section."
                    : NULL;
    if (!why) return f;
    LOGW("Presets: %s is %s; Not using it", from, why);
    cJSON_Delete(f);
    return NULL;
}

static cJSON *read_download(const char *name) {
    char path[PATH_MAX_LEN];
    download_path(name, path, sizeof path);
    char *txt = read_file(path, NULL);
    if (!txt) return NULL; /* A presets file was never downloaded. */
    cJSON *f = cJSON_Parse(txt);
    free(txt);
    return usable(f, name);
}

static void remove_downloads(void) {
    char path[PATH_MAX_LEN];
    download_path("current.json", path, sizeof path);
    unlink(path);
    download_path("previous.json", path, sizeof path);
    unlink(path);
}

void presets_init(void) {
    const char *embedded = plat_info()->emulator_catalog;
    cJSON *best = embedded ? usable (cJSON_Parse(embedded), "The built-in presets file") : NULL;
    cJSON *dl = read_download("current.json");
    if (!dl) dl = read_download("previous.json");
    if (dl && (!best || presets_version_cmp(jget_str(dl, "version", ""), jget_str(best, "version", "")) > 0)) {
        cJSON_Delete(best);
        best = dl;
        str_copy(g_source, sizeof g_source, "download");
    } else if (dl) {
        /* A newer RommPS brought newer presets than the last download */
        LOGI("presets: built-in %s is newer than the download (%s), let's remove it", jget_str(best, "version", ""), jget_str(dl, "version", ""));
        cJSON_Delete(dl);
        remove_downloads();
    }
    pthread_mutex_lock(&g_lock);
    g_raw = best;
    g_file = best ? resolved(best) : NULL;
    pthread_mutex_unlock(&g_lock);
    LOGI("presets: %s (%s)", best ? jget_str(best, "version", "") : "none", g_source);
}

/* The catalog filled in again, from the file as read. The one it replaces
 * stays, as with a download: a caller may still be reading it. */
void presets_refresh(void) {
    pthread_mutex_lock(&g_lock);
    if (g_raw) g_file = resolved(g_raw);
    pthread_mutex_unlock(&g_lock);
    LOGI("presets: filled in again for the machine's folders");
}

const cJSON *presets_catalog(void) {
    pthread_mutex_lock(&g_lock);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(g_file, "emulators");
    pthread_mutex_unlock(&g_lock);
    return c;
}

/* Where presets-<console-name>.json and .sig file are located. */
static const char *presets_url(void) {
    const char *u = getenv("ROMM_SYNC_PRESETS_URL"); /* Used for tests. */
    return u && *u ? u: PRESETS_URL;
}

static int fetch(const char *url, http_resp *r, char *err, size_t en) {
    if (http_get_verified(url, r) != 0) snprintf( err, en, "Can't reach %s: %s", url, r->error);
    else if (r->status != 200) snprintf(err, en, "%s answered with HTTP %ld", url, r->status);
    else return 0;
    return -1;
}

/*  Check to see if the presets file is signed by our release key */
static int signed_by_us(const http_resp *file, const http_resp *sig) {
    if (sig->len != 64) return 0;
    for (size_t i = 0; i < sizeof UPDATE_KEYS / sizeof UPDATE_KEYS[0]; i++)
        if (crypto_ed25519_check((const uint8_t *)sig->body, UPDATE_KEYS[i], (const uint8_t *)file->body, file->len) == 0)
            return 1;
    return 0;
}

static int save_download(const http_resp *file, char *err, size_t en) {
    char cur[PATH_MAX_LEN], prev[PATH_MAX_LEN];
    download_path("current.json", cur, sizeof cur);
    download_path("previous.json", prev, sizeof prev);
    mkdir_parent(cur);
    if (file_exists(cur) && move_file(cur, prev) !=0) LOGW("presets: can't keep the previous download");
    if (write_file_atomic(cur, file->body, file->len) == 0) return 0;
    snprintf(err, en, "can't save the presets to %s", cur);
    return -1;
}

static void apply(cJSON *f) {
    pthread_mutex_lock(&g_lock);
    g_raw = f;
    g_file = resolved(f);
    str_copy(g_source, sizeof g_source, "download");
    pthread_mutex_unlock(&g_lock);
    config_lock();
    int changed = g_cfg.profiles_custom &&  detect_refresh_profiles(g_cfg.profiles) > 0;
    if (changed) config_save();
    config_unlock();
    LOGI("presets: now %s%s", jget_str(f, "version", ""), changed ? ", saved profiles updated" : "");
}

static void *check_thread(void *arg) {
    (void)arg;
    char url[600], sig_url[600], have[32], err[256] = "";
    http_resp file = {0}, sig = {0};
    cJSON *f = NULL;

    snprintf(url, sizeof url, "%s/presets-%s.json", presets_url(), plat_info()->name);
    snprintf(sig_url, sizeof sig_url, "%s/presets-%s.sig", presets_url(), plat_info()->name);

    if(fetch(url, &file, err, sizeof err) == 0  && fetch(sig_url, &sig, err, sizeof err) == 0) {
        if (!signed_by_us(&file, &sig)) snprintf(err, sizeof err, "the presets signature doesn't match, so not using them");
        else if (!(f = usable(cJSON_ParseWithLength(file.body, file.len), "the download")))
            snprintf(err, sizeof err, "the published presets can't be used here");
    }
    if (f) {
        pthread_mutex_lock(&g_lock);
        str_copy(have, sizeof have, g_file ? jget_str(g_file, "version", "0") : "0");
        pthread_mutex_unlock(&g_lock);
        if (presets_version_cmp(jget_str(f, "version", ""), have) <= 0) {
            LOGI("presets: %s is current", have);
            cJSON_Delete(f);
        } else if (save_download(&file, err, sizeof err) == 0) {
            apply(f);
        } else {
            cJSON_Delete(f);
        }

    }
    if (err[0]) LOGW("presets: %s", err);
    http_resp_free(&file);
    http_resp_free(&sig);

    pthread_mutex_lock(&g_lock);
    g_checking = 0;
    g_checked_at = time(NULL);
    str_copy(g_error, sizeof g_error, err);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int presets_check(void) {
    pthread_mutex_lock(&g_lock);
    int busy = g_checking;
    g_checking = 1;
    pthread_mutex_unlock(&g_lock);
    if (busy) return -1;
    if (thread_start(check_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_checking = 0;
        str_copy(g_error, sizeof g_error, "cannot start the presets check");
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    return 0;
}

cJSON *presets_status(void) {
    cJSON *j = cJSON_CreateObject();
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(j, "version", g_file ? jget_str(g_file, "version", "") : "");
    cJSON_AddStringToObject(j, "source", g_source);
    cJSON_AddStringToObject(j, "state", g_checking ? "checking" : g_error[0] ? "error" : "idle");
    cJSON_AddStringToObject(j, "error", g_error);
    cJSON_AddNumberToObject(j, "checked_at", (double)g_checked_at);
    pthread_mutex_unlock(&g_lock);
    return j;
}
