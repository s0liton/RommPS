#include "cfgvars.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A value as written: a quoted one up to its closing quote, whatever follows
 * it (Xenia's toml: content_root = ""   # Root path for...); else, where
 * comments say so (toml, yaml), up to a '#' after a space; spaces around off. */
static void copy_value_c(const char *v, size_t len, int comments, char *out, size_t n) {
    while (len && (*v == ' ' || *v == '\t')) v++, len--;
    const char *close = len >= 2 && (v[0] == '"' || v[0] == '\'') ? memchr(v + 1, v[0], len - 1) : NULL;
    if (close) {
        len = (size_t)(close - v - 1);
        v++;
    } else if (comments) {
        for (size_t i = 1; i < len; i++)
            if (v[i] == '#' && (v[i - 1] == ' ' || v[i - 1] == '\t')) {
                len = i;
                break;
            }
    }
    while (len && !close && (v[len - 1] == ' ' || v[len - 1] == '\t' || v[len - 1] == '\r')) len--;
    if (len >= n) len = n - 1;
    memcpy(out, v, len);
    out[len] = 0;
}

static void copy_value(const char *v, size_t len, char *out, size_t n) { copy_value_c(v, len, 0, out, n); }

static int hex(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }

/* One character of a section name, %XX decoded (Qt writes spaces as %20). */
static int next_char(const char **s, const char *end) {
    const char *p = *s;
    if (p + 2 < end && p[0] == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
        *s = p + 3;
        return hex(p[1]) * 16 + hex(p[2]);
    }
    *s = p + 1;
    return (unsigned char)*p;
}

static int section_eq(const char *a, size_t al, const char *b) {
    const char *ae = a + al, *be = b + strlen(b);
    while (a < ae && b < be)
        if (next_char(&a, ae) != next_char(&b, be)) return 0;
    return a == ae && b == be;
}

/* ini, toml and MAME's "key value" lines. */
static int lookup_lines(const char *text, int space, int comments, const char *section, const char *key, char *out,
                        size_t n) {
    const int want_section = section && *section;
    const char *sec = NULL;
    size_t sl = 0, kl = strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        while (*l == ' ' || *l == '\t') l++;
        size_t ll = strcspn(l, "\n");
        if (*l == '[') {
            const char *end = memchr(l, ']', ll);
            if (end) sec = l + 1, sl = (size_t)(end - l - 1);
            continue;
        }
        if (*l == ';' || *l == '#') continue;
        if (want_section && (!sec || !section_eq(sec, sl, section))) continue;
        if (ll <= kl || strncmp(l, key, kl) != 0) continue;
        const char *v = l + kl;
        if (space) {
            if (*v != ' ' && *v != '\t') continue;
        } else {
            while (*v == ' ' || *v == '\t') v++;
            if (*v != '=') continue;
            v++;
        }
        copy_value_c(v, ll - (size_t)(v - l), comments, out, n);
        return 1;
    }
    return 0;
}

static size_t indent_of(const char *l) {
    size_t i = 0;
    while (l[i] == ' ' || l[i] == '\t') i++;
    return i;
}

/* l (indent skipped) starts with "name:" or "'name':" or "\"name\":"; the value after it. */
static const char *yaml_key(const char *l, const char *name) {
    size_t nl = strlen(name);
    char q = (*l == '"' || *l == '\'') ? *l : 0;
    if (q) l++;
    if (strncmp(l, name, nl) != 0) return NULL;
    l += nl;
    if (q && *l++ != q) return NULL;
    if (*l != ':' || (l[1] && l[1] != ' ' && l[1] != '\t' && l[1] != '\n' && l[1] != '\r')) return NULL;
    return l + 1;
}

static int lookup_yaml(const char *text, const char *section, const char *key, char *out, size_t n) {
    const char *l = text;
    size_t sec_indent = 0;
    int inside = !(section && *section);
    for (; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        size_t in = indent_of(l);
        const char *t = l + in;
        size_t tl = strcspn(t, "\n");
        if (!tl || *t == '#' || *t == '\r') continue;
        if (!inside) {
            /* "Paths:" in yaml; ares's and bsnes's BML write the bare name. */
            size_t sl = strlen(section);
            const int bare = !strncmp(t, section, sl) && strspn(t + sl, " \t\r") == strcspn(t + sl, "\n");
            if (yaml_key(t, section) || bare) inside = 1, sec_indent = in;
            continue;
        }
        if (section && *section && in <= sec_indent) return 0;
        const char *v = yaml_key(t, key);
        if (!v) continue;
        copy_value_c(v, tl - (size_t)(v - t), 1, out, n);
        return 1;
    }
    return 0;
}

static int lookup_xml(const char *text, const char *key, char *out, size_t n) {
    const char *at = text;
    char name[128];
    while (*key) {
        size_t cl = strcspn(key, "/");
        if (!cl || cl >= sizeof name) return 0;
        memcpy(name, key, cl);
        name[cl] = 0;
        key += cl + (key[cl] == '/');
        const char *open = at;
        for (;;) {
            open = strchr(open, '<');
            if (!open) return 0;
            if (!strncmp(open + 1, name, cl) && strchr(" \t\r\n/>", open[1 + cl])) break;
            open++;
        }
        const char *gt = strchr(open, '>');
        if (!gt) return 0;
        if (gt[-1] == '/') { /* <name/>: there, and empty */
            if (*key) return 0;
            out[0] = 0;
            return 1;
        }
        at = gt + 1;
    }
    const char *end = strchr(at, '<');
    if (!end) return 0;
    size_t o = 0;
    for (const char *p = at; p < end && o < n - 1;) {
        static const struct {
            const char *ent;
            char c;
        } ENT[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        size_t i = 0;
        for (; *p == '&' && i < sizeof ENT / sizeof ENT[0]; i++)
            if (!strncmp(p, ENT[i].ent, strlen(ENT[i].ent))) break;
        if (*p == '&' && i < sizeof ENT / sizeof ENT[0]) {
            out[o++] = ENT[i].c;
            p += strlen(ENT[i].ent);
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    char tmp[4096];
    if (o < sizeof tmp) {
        memcpy(tmp, out, o + 1);
        copy_value(tmp, o, out, n);
    }
    return 1;
}

int cfgvars_lookup(const char *text, const char *format, const char *section, const char *key, char *out, size_t n) {
    if (!text || !format || !key || !*key || !n) return 0;
    out[0] = 0;
    if (!strcmp(format, "ini")) return lookup_lines(text, 0, 0, section, key, out, n);
    if (!strcmp(format, "toml")) return lookup_lines(text, 0, 1, section, key, out, n);
    if (!strcmp(format, "space")) return lookup_lines(text, 1, 0, section, key, out, n);
    if (!strcmp(format, "yaml")) return lookup_yaml(text, section, key, out, n);
    if (!strcmp(format, "xml")) return lookup_xml(text, key, out, n);
    return 0;
}

void cfgvars_normalize(char *path) {
    const int abs = path[0] == '/';
    char *seg[256];
    int ns = 0;
    char *save = NULL;
    for (char *s = strtok_r(path, "/", &save); s && ns < 256; s = strtok_r(NULL, "/", &save)) {
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..") && ns && strcmp(seg[ns - 1], "..") != 0) {
            ns--;
            continue;
        }
        if (!strcmp(s, "..") && abs) continue; /* above / is / */
        seg[ns++] = s;
    }
    /* Segments point into path: build the result in a copy. */
    char buf[4096];
    size_t o = 0;
    if (abs) buf[o++] = '/';
    for (int i = 0; i < ns && o < sizeof buf - 1; i++) {
        size_t l = strlen(seg[i]);
        if (o + l + 1 >= sizeof buf) break;
        if (i) buf[o++] = '/';
        memcpy(buf + o, seg[i], l);
        o += l;
    }
    buf[o] = 0;
    memcpy(path, buf, o + 1);
}

/* ---- writing: config_writes ---------------------------------------------- */

/* A section's lines: where its body starts, and where it ends (after its last
 * line that isn't blank, so what's added sits with it). "" is the top of the
 * file, before the first section. found: the section is there. */
typedef struct {
    size_t body, end;
    int found;
} span;

static int blank_line(const char *l, size_t ll) {
    for (size_t i = 0; i < ll; i++)
        if (l[i] != ' ' && l[i] != '\t' && l[i] != '\r') return 0;
    return 1;
}

static span find_section(const char *text, const char *section) {
    const int top = !section || !*section;
    span s = {0, 0, top};
    int inside = top;
    for (const char *l = text; *l;) {
        size_t ll = strcspn(l, "\n");
        const size_t next = (size_t)(l - text) + ll + (l[ll] == '\n');
        const char *t = l;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '[') {
            if (inside) return s;
            const char *end = memchr(t, ']', ll - (size_t)(t - l));
            if (end && section_eq(t + 1, (size_t)(end - t - 1), section)) {
                inside = s.found = 1;
                s.body = s.end = next;
            }
        } else if (inside && !blank_line(l, ll)) {
            s.end = next;
        }
        l = text + next;
    }
    return s;
}

/* The next line from `from` on, before `to`, that sets key ("key=value",
 * "key = value"): its start and end (before the newline), and where its value
 * starts. "key\default=..." isn't key. 1 if there's one. */
static int find_key(const char *text, size_t from, size_t to, const char *key, size_t *ls, size_t *le, size_t *vs) {
    const size_t kl = strlen(key);
    for (size_t at = from; at < to && text[at];) {
        const char *l = text + at;
        size_t ll = strcspn(l, "\n");
        const size_t next = at + ll + (l[ll] == '\n');
        const char *t = l;
        while (*t == ' ' || *t == '\t') t++;
        if (*t != ';' && *t != '#' && !strncmp(t, key, kl)) {
            const char *v = t + kl;
            while (*v == ' ' || *v == '\t') v++;
            if (*v == '=') {
                v++;
                while (*v == ' ' || *v == '\t') v++;
                *ls = at;
                *le = at + ll - (ll && l[ll - 1] == '\r');
                *vs = (size_t)(v - text);
                if (*vs > *le) *vs = *le;
                return 1;
            }
        }
        at = next;
    }
    return 0;
}

/* Two values the same, quotes and spaces off; paths compared normalized. */
static int same_value(const char *a, size_t al, const char *b) {
    char x[4096], y[4096];
    copy_value(a, al, x, sizeof x);
    copy_value(b, strlen(b), y, sizeof y);
    if (x[0] == '/' && y[0] == '/') {
        cfgvars_normalize(x);
        cfgvars_normalize(y);
    }
    return !strcmp(x, y);
}

/* text with [from, to) replaced by ins: a new string. */
static char *splice(const char *text, size_t from, size_t to, const char *ins) {
    const size_t tl = strlen(text), il = strlen(ins);
    char *out = malloc(tl - (to - from) + il + 1);
    if (!out) return NULL;
    memcpy(out, text, from);
    memcpy(out + from, ins, il);
    memcpy(out + from + il, text + to, tl - to + 1);
    return out;
}

/* lines (each ending in a newline) added at the end of section, which is
 * made at the end of the file when it isn't there. */
static char *add_lines(const char *text, const char *section, const char *lines) {
    span s = find_section(text, section);
    const size_t tl = strlen(text);
    char *ins, *out;
    if (s.found) {
        const int nl = s.end > 0 && text[s.end - 1] != '\n';
        ins = malloc(strlen(lines) + 2);
        if (!ins) return NULL;
        strcpy(ins, nl ? "\n" : "");
        strcat(ins, lines);
        out = splice(text, s.end, s.end, ins);
    } else {
        const size_t n = strlen(section) + strlen(lines) + 8;
        ins = malloc(n);
        if (!ins) return NULL;
        snprintf(ins, n, "%s%s[%s]\n%s", tl && text[tl - 1] != '\n' ? "\n" : "", tl ? "\n" : "", section, lines);
        out = splice(text, tl, tl, ins);
    }
    free(ins);
    return out;
}

/* The value key has in section, into out. 1 if it's there. */
static int section_value(const char *text, const span *s, const char *key, char *out, size_t n) {
    size_t ls, le, vs;
    if (!s->found || !find_key(text, s->body, s->end, key, &ls, &le, &vs)) return 0;
    copy_value(text + vs, le - vs, out, n);
    return 1;
}

static const char *row_value(const cfgvars_row *r, const char *key) {
    for (int i = 0; r && i < r->n; i++)
        if (!strcmp(r->kv[i].key, key)) return r->kv[i].value;
    return NULL;
}

char *cfgvars_array_add(const char *text, const char *section, const char *array, const char *match,
                        const cfgvars_row *seed, int nseed, const cfgvars_row *add) {
    const char *want = row_value(add, match);
    if (!text || !add || !want || !*want) return NULL;
    span s = find_section(text, section);
    char key[512], val[4096];
    snprintf(key, sizeof key, "%s\\size", array);
    long size = section_value(text, &s, key, val, sizeof val) ? strtol(val, NULL, 10) : 0;
    if (size < 0) size = 0;
    for (long i = 1; i <= size; i++) {
        snprintf(key, sizeof key, "%s\\%ld\\%s", array, i, match);
        size_t ls, le, vs;
        if (s.found && find_key(text, s.body, s.end, key, &ls, &le, &vs) && same_value(text + vs, le - vs, want))
            return NULL;
    }
    /* The rows, numbered on from the last one there (from 1 when it's empty). */
    size_t cap = 256, o = 0;
    for (int r = 0; r < nseed; r++)
        for (int k = 0; k < seed[r].n; k++) cap += strlen(array) + strlen(seed[r].kv[k].key) + strlen(seed[r].kv[k].value) + 32;
    for (int k = 0; k < add->n; k++) cap += strlen(array) + strlen(add->kv[k].key) + strlen(add->kv[k].value) + 32;
    char *lines = malloc(cap);
    if (!lines) return NULL;
    lines[0] = 0;
    long at = size;
    for (int r = 0; size == 0 && r <= nseed; r++) {
        const cfgvars_row *row = r < nseed ? &seed[r] : add;
        at++;
        for (int k = 0; k < row->n; k++)
            o += (size_t)snprintf(lines + o, cap - o, "%s\\%ld\\%s=%s\n", array, at, row->kv[k].key, row->kv[k].value);
    }
    if (size > 0) {
        at++;
        for (int k = 0; k < add->n; k++)
            o += (size_t)snprintf(lines + o, cap - o, "%s\\%ld\\%s=%s\n", array, at, add->kv[k].key, add->kv[k].value);
    }
    /* Its size: the line there says so now, or one more goes in. */
    char *out;
    size_t ls, le, vs;
    snprintf(key, sizeof key, "%s\\size", array);
    if (s.found && find_key(text, s.body, s.end, key, &ls, &le, &vs)) {
        char num[24];
        snprintf(num, sizeof num, "%ld", at);
        char *sized = splice(text, vs, le, num);
        out = sized ? add_lines(sized, section, lines) : NULL;
        free(sized);
    } else {
        o += (size_t)snprintf(lines + o, cap - o, "%s=%ld\n", key, at);
        out = add_lines(text, section, lines);
    }
    free(lines);
    return out;
}

char *cfgvars_list_add(const char *text, const char *section, const char *key, const char *value) {
    if (!text || !value || !*value) return NULL;
    span s = find_section(text, section);
    size_t ls, le, vs;
    for (size_t at = s.body; s.found && find_key(text, at, s.end, key, &ls, &le, &vs); at = le + 1)
        if (same_value(text + vs, le - vs, value)) return NULL;
    const size_t n = strlen(key) + strlen(value) + 8;
    char *line = malloc(n);
    if (!line) return NULL;
    snprintf(line, n, "%s = %s\n", key, value);
    char *out = add_lines(text, section, line);
    free(line);
    return out;
}

char *cfgvars_set(const char *text, const char *section, const char *key, const char *value, const char *sep, int quote,
                  int if_unset) {
    if (!text || !value || !*value || strchr(value, '"') || strchr(value, '\n')) return NULL;
    const size_t n = strlen(key) + strlen(value) + (sep ? strlen(sep) : 3) + 8;
    char *v = malloc(n);
    if (!v) return NULL;
    snprintf(v, n, quote ? "\"%s\"" : "%s", value);
    span s = find_section(text, section);
    size_t ls, le, vs;
    char *out = NULL;
    if (s.found && find_key(text, s.body, s.end, key, &ls, &le, &vs)) {
        char now[4096];
        copy_value(text + vs, le - vs, now, sizeof now);
        if (!(if_unset && now[0]) && !same_value(text + vs, le - vs, value)) out = splice(text, vs, le, v);
    } else {
        char *line = malloc(n);
        if (line) {
            snprintf(line, n, "%s%s%s\n", key, sep ? sep : " = ", v);
            out = add_lines(text, section, line);
        }
        free(line);
    }
    free(v);
    return out;
}

/* Past the JSON string starting at s (on its quote), into out when given. */
static const char *json_str(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (s++; *s && *s != '"'; s++) {
        if (*s == '\\' && s[1]) s++;
        if (out && o + 1 < n) out[o++] = *s;
    }
    if (out && n) out[o] = 0;
    return *s ? s + 1 : s;
}

static const char *json_ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

char *cfgvars_json_array_add(const char *text, const char *key, const char *value) {
    if (!text || !value || !*value) return NULL;
    for (const char *c = value; *c; c++)
        if ((unsigned char)*c < 0x20) return NULL;
    /* The key at the top level: depth 1, outside strings. */
    const char *at = NULL, *line = text;
    int depth = 0;
    char name[256];
    for (const char *s = text; *s && !at;) {
        if (*s == '\n') line = s + 1;
        if (*s == '"') {
            const char *end = json_str(s, name, sizeof name);
            const char *colon = json_ws(end);
            if (depth == 1 && *colon == ':' && !strcmp(name, key)) at = json_ws(colon + 1);
            s = end;
            continue;
        }
        if (*s == '{' || *s == '[') depth++;
        else if (*s == '}' || *s == ']') depth--;
        s++;
    }
    if (!at || *at != '[') return NULL;
    /* Its strings: one that's value already, and where the array ends. */
    const char *s = json_ws(at + 1);
    const int empty = *s == ']';
    while (*s && *s != ']') {
        if (*s != '"') return NULL; /* not an array of strings */
        char have[4096];
        s = json_str(s, have, sizeof have);
        if (same_value(have, strlen(have), value)) return NULL;
        s = json_ws(s);
        if (*s == ',') s = json_ws(s + 1);
    }
    if (*s != ']') return NULL;
    size_t indent = 0;
    while (line[indent] == ' ' || line[indent] == '\t') indent++;
    char esc[4096 * 2];
    size_t o = 0;
    for (const char *c = value; *c && o + 2 < sizeof esc; c++) {
        if (*c == '"' || *c == '\\') esc[o++] = '\\';
        esc[o++] = *c;
    }
    esc[o] = 0;
    const size_t n = strlen(esc) + 2 * indent + 16;
    char *ins = malloc(n);
    if (!ins) return NULL;
    char *out;
    if (empty) {
        snprintf(ins, n, "[\n%*s  \"%s\"\n%*s]", (int)indent, "", esc, (int)indent, "");
        out = splice(text, (size_t)(at - text), (size_t)(s - text) + 1, ins);
    } else {
        snprintf(ins, n, "\n%*s  \"%s\",", (int)indent, "", esc);
        out = splice(text, (size_t)(at - text) + 1, (size_t)(at - text) + 1, ins);
    }
    free(ins);
    return out;
}
