#include "duckcard.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cfgvars.h"
#include "sigil.h"
#include "util.h"

/* ---- names ---------------------------------------------------------------- */

void duck_sanitize(char *name) {
    /* Path::SanitizeFileName, on Linux: whole UTF-8 characters stay, so only
     * these single bytes change. */
    for (unsigned char *p = (unsigned char *)name; *p; p++)
        if (*p == '/' || *p == '*' || *p < 0x20) *p = '_';
}

/* <name>_<slot>, sanitized. */
static void card_name(char *out, size_t n, const char *name, int slot) {
    char s[256];
    str_copy(s, sizeof s, name);
    duck_sanitize(s);
    snprintf(out, n, "%s_%d", s, slot);
}

static int card_exists(const char *dir, const char *stem) {
    char path[PATH_MAX_LEN], file[PATH_MAX_LEN];
    snprintf(file, sizeof file, "%s.mcd", stem);
    path_join(path, sizeof path, dir, file);
    return file_exists(path);
}

/* stem without " (Disc 1)": the name Redump gives the whole set. */
static void without_disc(const char *stem, char *out, size_t n) {
    str_copy(out, n, stem);
    char *at = strstr(out, " (Disc ");
    char *close = at ? strchr(at, ')') : NULL;
    if (close) memmove(at, close + 1, strlen(close + 1) + 1);
}

void duck_fallback(const char *dir, const char *stem, const char *serial, int slot, char *out, size_t n) {
    char cand[3][300], set[256];
    without_disc(stem, set, sizeof set);
    card_name(cand[0], sizeof cand[0], stem, slot);
    card_name(cand[1], sizeof cand[1], set, slot);
    if (serial && serial[0]) card_name(cand[2], sizeof cand[2], serial, slot);
    else cand[2][0] = 0;
    for (int i = 0; i < 3; i++)
        if (cand[i][0] && cand[i][0] != '_' && card_exists(dir, cand[i])) {
            str_copy(out, n, cand[i]);
            return;
        }
    /* A disc of a set: DuckStation gives the set one card, by its title. */
    str_copy(out, n, stem[0] ? cand[strcmp(set, stem) != 0] : cand[2]);
}

/* ---- settings -------------------------------------------------------------- */

/* StringUtil::FromChars<bool>: true, yes, on, 1. */
static int duck_bool(const char *v, int def) {
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) return 1;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) return 0;
    return def;
}

static int known_type(const char *v) {
    static const char *const types[] = {"None",         "Shared",           "PerGame",
                                        "PerGameTitle", "PerGameFileTitle", "NonPersistent"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (!strcmp(v, types[i])) return 1;
    return 0;
}

/* One layer's [MemoryCards] over what's in out. */
static void slot_layer(const char *txt, int slot, duck_slot *out) {
    char key[16], v[64];
    snprintf(key, sizeof key, "Card%dType", slot);
    if (cfgvars_lookup(txt, "ini", "MemoryCards", key, v, sizeof v) && known_type(v))
        str_copy(out->type, sizeof out->type, v);
    if (cfgvars_lookup(txt, "ini", "MemoryCards", "UsePlaylistTitle", v, sizeof v))
        out->playlist_title = duck_bool(v, out->playlist_title);
}

void duck_read_slot(const char *data_root, const char *ini_serial, int slot, duck_slot *out) {
    str_copy(out->type, sizeof out->type, slot == 1 ? "PerGameTitle" : "None");
    out->playlist_title = 1;
    char path[PATH_MAX_LEN], dir[PATH_MAX_LEN], v[PATH_MAX_LEN];
    path_join(path, sizeof path, data_root, "settings.ini");
    path_join(dir, sizeof dir, data_root, "gamesettings");
    char *txt = read_file(path, NULL);
    if (txt) {
        slot_layer(txt, slot, out);
        if (cfgvars_lookup(txt, "ini", "Folders", "GameSettings", v, sizeof v) && v[0]) {
            if (v[0] == '/') str_copy(dir, sizeof dir, v);
            else path_join(dir, sizeof dir, data_root, v);
        }
        free(txt);
    }
    if (!ini_serial || !ini_serial[0]) return;
    char name[64];
    snprintf(name, sizeof name, "%s.ini", ini_serial);
    duck_sanitize(name);
    path_join(path, sizeof path, dir, name);
    if ((txt = read_file(path, NULL))) {
        slot_layer(txt, slot, out);
        free(txt);
    }
}

/* What the user set for a game in DuckStation's list (custom_properties.ini,
 * a section per file path): its title, its serial. */
static void custom_props(const char *data_root, const char *boot, char *title, size_t tn, char *serial, size_t sn) {
    title[0] = serial[0] = 0;
    /* A path with [ or ] in it is kept URL-encoded: not read here. */
    if (!data_root || strpbrk(boot, "[]")) return;
    char path[PATH_MAX_LEN];
    path_join(path, sizeof path, data_root, "custom_properties.ini");
    char *txt = read_file(path, NULL);
    if (!txt) return;
    if (!cfgvars_lookup(txt, "ini", boot, "Title", title, tn)) title[0] = 0;
    if (!cfgvars_lookup(txt, "ini", boot, "Serial", serial, sn)) serial[0] = 0;
    free(txt);
}

/* ---- the database ---------------------------------------------------------- */

typedef struct {
    char *title, *first;
} dset;
typedef struct {
    char *serial, *title;
    int set;
} dent;
typedef struct {
    char *code;
    int ent, order;
} dcode;
typedef struct {
    char *serial;
    int set;
} dmember;

struct duck_db {
    dent *e;
    dset *s;
    dcode *c;
    dmember *m;
    int ne, ns, nc, nm, legacy;
    size_t ce, cs, cc, cm;
};

/* Grows an array for one more. 0 when there's no memory. */
static int grow(void **arr, size_t *cap, int n, size_t size) {
    if ((size_t)n < *cap) return 1;
    size_t want = *cap ? *cap * 2 : 256;
    void *g = realloc(*arr, want * size);
    if (!g) return 0;
    *arr = g;
    *cap = want;
    return 1;
}

static char *dup_str(const char *s) {
    size_t l = strlen(s) + 1;
    char *d = malloc(l);
    if (d) memcpy(d, s, l);
    return d;
}

/* A YAML scalar as written: "double quoted" (\" and \\ in it), 'single
 * quoted' ('' for '), or plain up to a comment. */
static void yaml_scalar(const char *v, size_t len, char *out, size_t n) {
    size_t o = 0;
    while (len && (*v == ' ' || *v == '\t')) v++, len--;
    while (len && (v[len - 1] == ' ' || v[len - 1] == '\t' || v[len - 1] == '\r')) len--;
    if (len && (*v == '"' || *v == '\'')) {
        const char q = *v;
        for (size_t i = 1; i < len && o + 1 < n; i++) {
            if (q == '"' && v[i] == '\\' && i + 1 < len) {
                out[o++] = v[++i];
                continue;
            }
            if (v[i] == q) {
                if (q == '\'' && i + 1 < len && v[i + 1] == '\'') {
                    out[o++] = v[++i];
                    continue;
                }
                break;
            }
            out[o++] = v[i];
        }
    } else {
        for (size_t i = 1; i < len; i++)
            if (v[i] == '#' && (v[i - 1] == ' ' || v[i - 1] == '\t')) len = i;
        while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
        for (size_t i = 0; i < len && o + 1 < n; i++) out[o++] = v[i];
    }
    out[o] = 0;
}

/* One line, its indent taken off: "key: value" (key and value set), "- item"
 * (item set), or "key:" alone (value ""). 0 for a blank line or a comment. */
typedef struct {
    int indent, item;
    char key[64], value[512];
} yline;

static int yaml_line(const char *l, size_t ll, yline *y) {
    size_t i = 0;
    while (i < ll && l[i] == ' ') i++;
    if (i == ll || l[i] == '#' || l[i] == '\r') return 0;
    y->indent = (int)i;
    y->item = 0;
    y->key[0] = y->value[0] = 0;
    const char *t = l + i;
    size_t tl = ll - i;
    if (tl >= 2 && t[0] == '-' && (t[1] == ' ' || t[1] == '\t')) {
        y->item = 1;
        t += 2, tl -= 2;
        y->indent += 2; /* "- name: x": name sits two in, as its siblings do */
        while (tl && *t == ' ') t++, tl--;
    } else if (tl == 1 && t[0] == '-') {
        y->item = 1;
        return 1;
    }
    /* A key: up to ": " or a ':' at the end, outside quotes. */
    const char *colon = NULL;
    if (tl && *t != '"' && *t != '\'')
        for (size_t k = 0; k < tl; k++)
            if (t[k] == ':' && (k + 1 == tl || t[k + 1] == ' ' || t[k + 1] == '\t' || t[k + 1] == '\r')) {
                colon = t + k;
                break;
            }
    if (!colon) {
        yaml_scalar(t, tl, y->value, sizeof y->value); /* a list item's value */
        return 1;
    }
    yaml_scalar(t, (size_t)(colon - t), y->key, sizeof y->key);
    yaml_scalar(colon + 1, tl - (size_t)(colon - t) - 1, y->value, sizeof y->value);
    return 1;
}

/* The next line from *at (moved past it), its length in *ll; NULL at the end. */
static const char *next_line(const char **at, size_t *ll) {
    const char *l = *at;
    if (!*l) return NULL;
    *ll = strcspn(l, "\n");
    *at = l + *ll + (l[*ll] == '\n');
    return l;
}

static int add_code(duck_db *db, const char *code, int ent) {
    if (!code[0] || !grow((void **)&db->c, &db->cc, db->nc, sizeof *db->c)) return 0;
    char *c = dup_str(code);
    if (!c) return 0;
    db->c[db->nc] = (dcode){c, ent, db->nc};
    db->nc++;
    return 1;
}

static int add_set(duck_db *db, const char *title, const char *first) {
    if (!grow((void **)&db->s, &db->cs, db->ns, sizeof *db->s)) return -1;
    char *t = dup_str(title), *f = dup_str(first);
    if (!t || !f) {
        free(t);
        free(f);
        return -1;
    }
    db->s[db->ns] = (dset){t, f};
    return db->ns++;
}

/* The entry being read: done once the next starts. */
typedef struct {
    char serial[64], name[256], save[256], set_name[256], set_first[64];
    int codes;
} pending;

static void finish_entry(duck_db *db, pending *p) {
    if (!p->serial[0]) return;
    if (!grow((void **)&db->e, &db->ce, db->ne, sizeof *db->e)) return;
    dent *e = &db->e[db->ne];
    e->serial = dup_str(p->serial);
    e->title = dup_str(p->save[0] ? p->save : p->name);
    e->set = p->set_name[0] ? add_set(db, p->set_name, p->set_first) : -1;
    if (!e->serial || !e->title) {
        free(e->serial);
        free(e->title);
        return;
    }
    db->ne++;
    /* No codes: the serial is the only way to it. */
    if (!p->codes) add_code(db, p->serial, db->ne - 1);
    memset(p, 0, sizeof *p);
}

/* gamedb.yaml: an entry per serial ("SLUS-00594:" at the left), its name,
 * saveName and codes two in; an older build's discSet (name, serials) in it. */
static void parse_gamedb(duck_db *db, const char *text) {
    pending p;
    memset(&p, 0, sizeof p);
    enum { NONE, CODES, SET, SET_SERIALS } sec = NONE;
    yline y;
    const char *at = text, *l;
    size_t ll;
    while ((l = next_line(&at, &ll))) {
        if (!yaml_line(l, ll, &y)) continue;
        if (y.indent == 0 && !y.item && y.key[0]) {
            finish_entry(db, &p);
            str_copy(p.serial, sizeof p.serial, y.key);
            sec = NONE;
        } else if (!p.serial[0]) {
            continue;
        } else if (y.indent == 2 && !y.item) {
            sec = !strcmp(y.key, "codes") ? CODES : !strcmp(y.key, "discSet") ? SET : NONE;
            if (!strcmp(y.key, "name")) str_copy(p.name, sizeof p.name, y.value);
            else if (!strcmp(y.key, "saveName")) str_copy(p.save, sizeof p.save, y.value);
        } else if (sec == CODES && y.item && y.indent >= 4 && !y.key[0]) {
            p.codes += add_code(db, y.value, db->ne);
        } else if ((sec == SET || sec == SET_SERIALS) && !y.item && y.indent == 4) {
            sec = !strcmp(y.key, "serials") ? SET_SERIALS : SET;
            if (!strcmp(y.key, "name")) str_copy(p.set_name, sizeof p.set_name, y.value);
        } else if (sec == SET_SERIALS && y.item && y.indent >= 6 && !y.key[0] && !p.set_first[0]) {
            str_copy(p.set_first, sizeof p.set_first, y.value);
        }
    }
    finish_entry(db, &p);
}

/* discsets.yaml: a list of sets, each "- name:", "saveName:", "serials:". */
static void parse_discsets(duck_db *db, const char *text) {
    char name[256] = "", save[256] = "";
    int set = -1, in_serials = 0;
    yline y;
    const char *at = text, *l;
    size_t ll;
    while ((l = next_line(&at, &ll))) {
        if (!yaml_line(l, ll, &y)) continue;
        if (y.item && y.indent == 2 && y.key[0]) { /* "- name: ..." */
            name[0] = save[0] = 0;
            set = -1;
            in_serials = 0;
        }
        if (y.indent == 2 && y.key[0]) {
            in_serials = !strcmp(y.key, "serials");
            if (!strcmp(y.key, "name")) str_copy(name, sizeof name, y.value);
            else if (!strcmp(y.key, "saveName")) str_copy(save, sizeof save, y.value);
        } else if (in_serials && y.item && y.indent >= 4 && !y.key[0] && y.value[0]) {
            /* The title comes before the serials. */
            if (set < 0 && (set = add_set(db, save[0] ? save : name, y.value)) < 0) continue;
            if (!grow((void **)&db->m, &db->cm, db->nm, sizeof *db->m)) continue;
            char *s = dup_str(y.value);
            if (!s) continue;
            db->m[db->nm++] = (dmember){s, set};
        }
    }
}

static int code_cmp(const void *a, const void *b) {
    const dcode *x = a, *y = b;
    int c = strcmp(x->code, y->code);
    return c ? c : x->order - y->order;
}

static int member_cmp(const void *a, const void *b) {
    return strcmp(((const dmember *)a)->serial, ((const dmember *)b)->serial);
}

duck_db *duck_db_parse(const char *gamedb, const char *discsets) {
    duck_db *db = calloc(1, sizeof *db);
    if (!db) return NULL;
    db->legacy = discsets == NULL;
    if (discsets) parse_discsets(db, discsets);
    parse_gamedb(db, gamedb);
    if (db->nm) {
        qsort(db->m, (size_t)db->nm, sizeof *db->m, member_cmp);
        /* A set's discs are linked to it by their entries' serials. */
        for (int i = 0; i < db->ne; i++) {
            dmember key = {db->e[i].serial, 0};
            const dmember *m = bsearch(&key, db->m, (size_t)db->nm, sizeof *db->m, member_cmp);
            if (m) db->e[i].set = m->set;
        }
    }
    qsort(db->c, (size_t)db->nc, sizeof *db->c, code_cmp);
    /* A code twice: the first entry keeps it, as DuckStation does. */
    int k = 0;
    for (int i = 0; i < db->nc; i++) {
        if (k && !strcmp(db->c[k - 1].code, db->c[i].code)) {
            free(db->c[i].code);
            continue;
        }
        db->c[k++] = db->c[i];
    }
    db->nc = k;
    return db;
}

void duck_db_free(duck_db *db) {
    if (!db) return;
    for (int i = 0; i < db->ne; i++) {
        free(db->e[i].serial);
        free(db->e[i].title);
    }
    for (int i = 0; i < db->ns; i++) {
        free(db->s[i].title);
        free(db->s[i].first);
    }
    for (int i = 0; i < db->nc; i++) free(db->c[i].code);
    for (int i = 0; i < db->nm; i++) free(db->m[i].serial);
    free(db->e);
    free(db->s);
    free(db->c);
    free(db->m);
    free(db);
}

int duck_db_legacy(const duck_db *db) { return db && db->legacy; }

size_t duck_db_size(const duck_db *db) { return db ? (size_t)db->ne : 0; }

int duck_db_find(const duck_db *db, const char *code, duck_entry *out) {
    if (!db || !code || !code[0]) return 0;
    const dcode *c = NULL;
    size_t lo = 0, hi = (size_t)db->nc;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(db->c[mid].code, code);
        if (!r) {
            c = &db->c[mid];
            break;
        }
        if (r < 0) lo = mid + 1;
        else hi = mid;
    }
    if (!c || c->ent >= db->ne) return 0;
    const dent *e = &db->e[c->ent];
    memset(out, 0, sizeof *out);
    str_copy(out->serial, sizeof out->serial, e->serial);
    str_copy(out->title, sizeof out->title, e->title);
    if (e->set >= 0) {
        str_copy(out->set_title, sizeof out->set_title, db->s[e->set].title);
        str_copy(out->set_first, sizeof out->set_first, db->s[e->set].first);
    }
    return 1;
}

/* ---- where it is ----------------------------------------------------------- */

/* A file of /proc, whose size reads as 0: read to its end. */
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        char *g = realloc(buf, cap * 2 + 1);
        if (!g) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = g;
        cap *= 2;
    }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    *len = n;
    return buf;
}

/* A variable of a NUL-separated environment. */
static int env_value(const char *env, size_t len, const char *name, char *out, size_t n) {
    const size_t nl = strlen(name);
    for (size_t at = 0; at < len;) {
        const char *v = env + at;
        const size_t vl = strnlen(v, len - at);
        if (vl > nl && !strncmp(v, name, nl) && v[nl] == '=') {
            str_copy(out, n, v + nl + 1);
            return 1;
        }
        at += vl + 1;
    }
    return 0;
}

static void canonical(const char *in, char *out, size_t n) {
    char *real = realpath(in, NULL); /* its own buffer: a shorter one than PATH_MAX isn't allowed */
    str_copy(out, n, real ? real : in);
    free(real);
    cfgvars_normalize(out);
}

/* The data root a DuckStation with this environment uses (core.cpp
 * Core::SetDataRoot): beside its AppImage when portable, else
 * $XDG_CONFIG_HOME/duckstation, else ~/.local/share/duckstation. */
static int data_root_of(const char *env, size_t len, char *out, size_t n) {
    char v[PATH_MAX_LEN], dir[PATH_MAX_LEN], probe[PATH_MAX_LEN];
    if (env_value(env, len, "APPIMAGE", v, sizeof v)) {
        str_copy(dir, sizeof dir, v);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        path_join(probe, sizeof probe, dir, "portable.txt");
        int portable = file_exists(probe);
        path_join(probe, sizeof probe, dir, "settings.ini");
        if (portable || file_exists(probe)) {
            canonical(dir, out, n);
            return 1;
        }
    }
    if (env_value(env, len, "XDG_CONFIG_HOME", v, sizeof v) && v[0] == '/') {
        path_join(dir, sizeof dir, v, "duckstation");
    } else if (env_value(env, len, "HOME", v, sizeof v) && v[0]) {
        path_join(dir, sizeof dir, v, ".local/share/duckstation");
    } else {
        return 0;
    }
    canonical(dir, out, n);
    return 1;
}

static int read_root(const char *copy_dir, char *out, size_t n) {
    char path[PATH_MAX_LEN];
    path_join(path, sizeof path, copy_dir, "data_root");
    char *txt = read_file(path, NULL);
    if (!txt) return 0;
    txt[strcspn(txt, "\r\n")] = 0;
    str_copy(out, n, txt);
    free(txt);
    return out[0] != 0;
}

/* Only for the probe: one at a time, at most every PROBE_SEC. */
#define PROBE_SEC 10
static pthread_mutex_t g_probe_lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_one(const char *proc, const char *pid, const char *copies) {
    char link[PATH_MAX_LEN], exe[PATH_MAX_LEN];
    snprintf(link, sizeof link, "%s/%s/exe", proc, pid);
    ssize_t k = readlink(link, exe, sizeof exe - 1);
    if (k <= 0) return;
    exe[k] = 0;
    if (strncasecmp(path_basename(exe), "duckstation", 11) != 0) return;
    char res[PATH_MAX_LEN], gamedb[PATH_MAX_LEN], sets[PATH_MAX_LEN];
    str_copy(res, sizeof res, exe);
    char *slash = strrchr(res, '/');
    if (!slash) return;
    str_copy(slash, sizeof res - (size_t)(slash - res), "/resources");
    path_join(gamedb, sizeof gamedb, res, "gamedb.yaml");
    path_join(sets, sizeof sets, res, "discsets.yaml");
    /* A Flatpak's (/app/...) isn't this machine's path: its files are found
     * where Flatpak keeps them (duck_db_for's dirs). */
    if (!file_exists(gamedb)) return;
    char env_path[PATH_MAX_LEN], root[PATH_MAX_LEN], appimage[PATH_MAX_LEN] = "";
    snprintf(env_path, sizeof env_path, "%s/%s/environ", proc, pid);
    size_t elen = 0;
    char *env = slurp(env_path, &elen);
    if (!env) return;
    const int have_root = data_root_of(env, elen, root, sizeof root);
    env_value(env, elen, "APPIMAGE", appimage, sizeof appimage);
    free(env);
    if (!have_root) return;
    /* Keyed by the AppImage: one copy per version of it. */
    struct stat st;
    if (!(appimage[0] && stat(appimage, &st) == 0) && stat(gamedb, &st) != 0) return;
    char key[64], dest[PATH_MAX_LEN], have[PATH_MAX_LEN], to[PATH_MAX_LEN];
    snprintf(key, sizeof key, "%lld-%lld", (long long)st.st_size, (long long)st.st_mtime);
    path_join(dest, sizeof dest, copies, key);
    path_join(to, sizeof to, dest, "gamedb.yaml");
    if (file_exists(to) && read_root(dest, have, sizeof have) && !strcmp(have, root)) return;
    if (mkdir_p(dest) != 0 || copy_file(gamedb, to) != 0) {
        LOGW("couldn't copy DuckStation's game database from %s", res);
        return;
    }
    path_join(to, sizeof to, dest, "discsets.yaml");
    if (file_exists(sets)) copy_file(sets, to);
    else unlink(to);
    char line[PATH_MAX_LEN + 1];
    snprintf(line, sizeof line, "%s\n", root);
    path_join(to, sizeof to, dest, "data_root");
    write_file_atomic(to, line, strlen(line));
    LOGI("DuckStation's game database, for its memory card names: copied from %s (data in %s)",
         appimage[0] ? appimage : exe, root);
    /* The copies of older versions with the same data root go. */
    DIR *d = opendir(copies);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, key)) continue;
        char other[PATH_MAX_LEN];
        path_join(other, sizeof other, copies, de->d_name);
        if (read_root(other, have, sizeof have) && !strcmp(have, root)) remove_tree(other);
    }
    if (d) closedir(d);
}

void duck_probe_running(const char *proc, const char *copies) {
    static time_t last;
    pthread_mutex_lock(&g_probe_lock);
    const time_t now = time(NULL);
    if (now - last < PROBE_SEC && now >= last) {
        pthread_mutex_unlock(&g_probe_lock);
        return;
    }
    last = now;
    DIR *d = opendir(proc);
    struct dirent *de;
    while (d && (de = readdir(d)))
        if (isdigit((unsigned char)de->d_name[0])) copy_one(proc, de->d_name, copies);
    if (d) closedir(d);
    pthread_mutex_unlock(&g_probe_lock);
}

/* The databases read, while their files stay as they were. */
#define DBS_KEPT 4
static struct {
    char key[PATH_MAX_LEN + 96];
    duck_db *db;
} g_dbs[DBS_KEPT];
static int g_dbs_next;
static pthread_mutex_t g_dbs_lock = PTHREAD_MUTEX_INITIALIZER;

/* The newest copy taken from a DuckStation with this data root. */
static int find_copy(const char *copies, const char *data_root, char *out, size_t n) {
    char want[PATH_MAX_LEN], have[PATH_MAX_LEN], path[PATH_MAX_LEN];
    canonical(data_root, want, sizeof want);
    DIR *d = opendir(copies);
    if (!d) return 0;
    struct dirent *de;
    time_t best = 0;
    out[0] = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char dir[PATH_MAX_LEN];
        path_join(dir, sizeof dir, copies, de->d_name);
        path_join(path, sizeof path, dir, "gamedb.yaml");
        if (!read_root(dir, have, sizeof have) || strcmp(have, want) != 0 || !file_exists(path)) continue;
        const time_t t = file_mtime(path);
        if (!out[0] || t > best) {
            best = t;
            str_copy(out, n, dir);
        }
    }
    closedir(d);
    return out[0] != 0;
}

const duck_db *duck_db_for(const char *dirs, const char *data_root, const char *copies) {
    char src[PATH_MAX_LEN] = "", gamedb[PATH_MAX_LEN], sets[PATH_MAX_LEN];
    if (dirs && dirs[0]) {
        char list[PATH_MAX_LEN * 2], *save = NULL;
        str_copy(list, sizeof list, dirs);
        for (char *t = strtok_r(list, ",", &save); t && !src[0]; t = strtok_r(NULL, ",", &save)) {
            path_join(gamedb, sizeof gamedb, t, "gamedb.yaml");
            if (t[0] && !strchr(t, '{') && file_exists(gamedb)) str_copy(src, sizeof src, t);
        }
    }
    if (!src[0] && !(copies && data_root && data_root[0] && find_copy(copies, data_root, src, sizeof src))) return NULL;
    path_join(gamedb, sizeof gamedb, src, "gamedb.yaml");
    path_join(sets, sizeof sets, src, "discsets.yaml");
    char key[sizeof g_dbs[0].key];
    snprintf(key, sizeof key, "%s|%lld|%lld|%lld|%lld", src, (long long)file_size(gamedb),
             (long long)file_mtime_ns(gamedb), (long long)file_size(sets), (long long)file_mtime_ns(sets));
    pthread_mutex_lock(&g_dbs_lock);
    for (int i = 0; i < DBS_KEPT; i++)
        if (g_dbs[i].db && !strcmp(g_dbs[i].key, key)) {
            const duck_db *db = g_dbs[i].db;
            pthread_mutex_unlock(&g_dbs_lock);
            return db;
        }
    char *g = read_file(gamedb, NULL), *s = file_exists(sets) ? read_file(sets, NULL) : NULL;
    duck_db *db = g ? duck_db_parse(g, s) : NULL;
    free(g);
    free(s);
    if (!db || !db->ne) {
        duck_db_free(db);
        pthread_mutex_unlock(&g_dbs_lock);
        LOGW("DuckStation's game database in %s couldn't be read", src);
        return NULL;
    }
    /* The same folder's older read goes; otherwise the oldest. */
    int at = -1;
    const size_t sl = strlen(src);
    for (int i = 0; i < DBS_KEPT && at < 0; i++)
        if (g_dbs[i].db && !strncmp(g_dbs[i].key, src, sl) && g_dbs[i].key[sl] == '|') at = i;
    if (at < 0) at = g_dbs_next++ % DBS_KEPT;
    duck_db_free(g_dbs[at].db);
    g_dbs[at].db = db;
    str_copy(g_dbs[at].key, sizeof g_dbs[at].key, key);
    pthread_mutex_unlock(&g_dbs_lock);
    LOGI("DuckStation's game database: %s (%zu games%s)", src, duck_db_size(db), db->legacy ? ", an older build" : "");
    return db;
}

/* ---- a game's card --------------------------------------------------------- */

int duck_m3u_first(const char *m3u, char *out, size_t n) {
    char *txt = read_file(m3u, NULL);
    if (!txt) return -1;
    int rc = -1;
    const char *at = txt, *l;
    size_t ll;
    while ((l = next_line(&at, &ll))) {
        size_t b = 0, e = ll;
        while (b < e && isspace((unsigned char)l[b])) b++;
        while (e > b && isspace((unsigned char)l[e - 1])) e--;
        if (b == e || l[b] == '#') continue;
        char entry[PATH_MAX_LEN];
        snprintf(entry, sizeof entry, "%.*s", (int)(e - b), l + b);
        if (entry[0] == '/') {
            str_copy(out, n, entry);
        } else {
            char dir[PATH_MAX_LEN];
            str_copy(dir, sizeof dir, m3u);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
            else str_copy(dir, sizeof dir, ".");
            path_join(out, n, dir, entry);
        }
        rc = 0;
        break;
    }
    free(txt);
    return rc;
}

duck_kind duck_card_stem(const duck_db *db, const duck_game *g, int slot, char *out, size_t n, int *sure) {
    out[0] = 0;
    *sure = 1;
    char stem[256], custom_title[256], custom_serial[24];
    path_stem(stem, sizeof stem, path_basename(g->boot));
    custom_props(g->data_root, g->boot, custom_title, sizeof custom_title, custom_serial, sizeof custom_serial);
    const char *serial = custom_serial[0] ? custom_serial : g->serial ? g->serial : "";
    duck_entry en;
    const int known = duck_db_find(db, serial, &en);
    /* A set's settings are its first disc's. */
    duck_slot s;
    duck_read_slot(g->data_root, known ? (en.set_first[0] ? en.set_first : en.serial) : serial, slot, &s);
    if (!strcmp(s.type, "None") || !strcmp(s.type, "NonPersistent")) return DUCK_OFF;
    if (!strcmp(s.type, "Shared")) return DUCK_SHARED;
    const int m3u = str_ends_with_ci(g->boot, ".m3u"), pbp = str_ends_with_ci(g->boot, ".pbp");
    if (!strcmp(s.type, "PerGameFileTitle")) {
        card_name(out, n, stem, slot);
        return DUCK_PER_GAME;
    }
    if (!strcmp(s.type, "PerGame") && serial[0]) {
        /* The database's serial for a disc with several codes. */
        card_name(out, n, known ? en.serial : serial, slot);
        *sure = db != NULL;
        return DUCK_PER_GAME;
    }
    if (!db || pbp || !serial[0]) {
        /* What it reads that this can't: its database, a .pbp's discs, a disc
         * whose serial didn't read here (DuckStation may still know it). */
        *sure = 0;
        duck_fallback(g->card_dir, stem, serial, slot, out, n);
        return DUCK_PER_GAME;
    }
    /* PerGameTitle. RetroDECK's older build names a playlist's card after the
     * playlist's file, not the set, when that differs from the disc's title. */
    const int legacy = duck_db_legacy(db), playlist = legacy && m3u;
    const char *running = playlist ? stem : custom_title[0] ? custom_title : known ? en.title : stem;
    char set[300] = "", disc[300];
    if (playlist && known && strcmp(running, en.title) != 0) card_name(set, sizeof set, running, slot);
    else if (known && en.set_title[0]) card_name(set, sizeof set, en.set_title, slot);
    card_name(disc, sizeof disc, known && (playlist || !custom_title[0]) ? en.title : running, slot);
    /* One card for the set, unless the disc has one of its own already. */
    if (strcmp(disc, set) != 0 && (!set[0] || !s.playlist_title || card_exists(g->card_dir, disc)))
        str_copy(set, sizeof set, disc);
    str_copy(out, n, set);
    return DUCK_PER_GAME;
}

/* ---- by what's on the cards ------------------------------------------------ */

/* Whose saves each card holds, kept while the file stays as it is. */
#define OWNERS_KEPT 128
static struct {
    char path[PATH_MAX_LEN];
    int64_t mtime_ns, size;
    char *owners; /* "\n<id>\n<id>\n" */
} g_own[OWNERS_KEPT];
static int g_own_next;
static pthread_mutex_t g_own_lock = PTHREAD_MUTEX_INITIALIZER;

static int card_has(const char *path, const char *serial) {
    const int64_t mt = file_mtime_ns(path), sz = file_size(path);
    char want[64];
    snprintf(want, sizeof want, "\n%s\n", serial);
    pthread_mutex_lock(&g_own_lock);
    for (int i = 0; i < OWNERS_KEPT; i++)
        if (g_own[i].owners && !strcmp(g_own[i].path, path) && g_own[i].mtime_ns == mt && g_own[i].size == sz) {
            const int has = strstr(g_own[i].owners, want) != NULL;
            pthread_mutex_unlock(&g_own_lock);
            return has;
        }
    pthread_mutex_unlock(&g_own_lock);
    sigil_io *io = sigil_io_open_file(path);
    sigil_card_listing *l = NULL;
    char *owners = NULL;
    if (io && sigil_card_list(io, &l) == SIGIL_OK) {
        size_t len = 1;
        owners = malloc(l->entry_count * (SIGIL_CARD_OWNER_MAX + 1) + 2);
        if (owners) strcpy(owners, "\n");
        for (size_t i = 0; owners && i < l->entry_count; i++) {
            len += (size_t)sprintf(owners + len, "%s\n", l->entries[i].owner_id);
        }
    }
    sigil_card_listing_free(l);
    if (io) sigil_io_close(io);
    const int has = owners && strstr(owners, want) != NULL;
    if (owners && strlen(path) < sizeof g_own[0].path) {
        pthread_mutex_lock(&g_own_lock);
        int at = -1;
        for (int i = 0; i < OWNERS_KEPT && at < 0; i++)
            if (g_own[i].owners && !strcmp(g_own[i].path, path)) at = i;
        if (at < 0) at = g_own_next++ % OWNERS_KEPT;
        free(g_own[at].owners);
        str_copy(g_own[at].path, sizeof g_own[at].path, path);
        g_own[at].mtime_ns = mt;
        g_own[at].size = sz;
        g_own[at].owners = owners;
        owners = NULL;
        pthread_mutex_unlock(&g_own_lock);
    }
    free(owners);
    return has;
}

int duck_card_by_owner(const char *dir, int slot, const char *const *serials, int nserials, char *out, size_t n) {
    char tail[16];
    snprintf(tail, sizeof tail, "_%d.mcd", slot);
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !str_ends_with_ci(de->d_name, tail) || !strncasecmp(de->d_name, "shared_card_", 12))
            continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        int has = 0;
        for (int i = 0; i < nserials && !has; i++) has = serials[i] && serials[i][0] && card_has(path, serials[i]);
        if (!has) continue;
        if (found++ == 0) {
            str_copy(out, n, de->d_name);
            out[strlen(out) - 4] = 0; /* ".mcd" */
        }
    }
    closedir(d);
    if (found != 1) out[0] = 0;
    return found == 1;
}
