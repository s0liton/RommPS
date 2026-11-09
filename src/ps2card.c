#include "ps2card.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sigil.h"
#include "util.h"
#include "card.h"     /* third_party/sigil/src: sigil_card_sony_owner */
#include "card_ps2.h" /* sigil_ps2_card_format */

/* sigil's save root is the folder holding memcards/; the cards here may sit in
 * the emulator's folder itself (PS5SX2 without a memcards/ folder), so every
 * path sigil names is mapped onto ps2_cards.dir. */
#define ROOT "memcards/"

/* sigil.c resolves every platform slug, and with it links every disc reader;
 * only the sync code is taken here, and it only asks for "ps2".
 * shortcut: knows "ps2" only, take sigil.c if another platform syncs through sigil. */
sigil_platform sigil_platform_from_slug(const char *slug) {
    return slug && !strcmp(slug, "ps2") ? SIGIL_PLATFORM_PS2 : SIGIL_PLATFORM_AUTO;
}

typedef struct {
    char **items;
    size_t n, cap;
} str_list;

static int list_add(str_list *l, const char *s) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        char **grown = realloc(l->items, cap * sizeof *grown);
        if (!grown) return -1;
        l->items = grown;
        l->cap = cap;
    }
    if (!(l->items[l->n] = strdup(s))) return -1;
    l->n++;
    return 0;
}

static void list_free(str_list *l) {
    for (size_t i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
}

/* Every file below dir, as "<rel>/<name>". */
static int list_tree(str_list *l, const char *dir, const char *rel, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN], name[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        snprintf(name, sizeof name, "%s/%s", rel, de->d_name);
        if (dir_exists(path)) rc = depth < 4 ? list_tree(l, path, name, depth + 1) : 0;
        else rc = list_add(l, name);
    }
    closedir(d);
    return rc;
}

static int list_cards(const ps2_cards *c, str_list *l) {
    for (int s = 0; s < 2; s++) {
        const char *card = c->slot[s];
        if (!card[0] || strchr(card, '/') || (s == 1 && !strcmp(card, c->slot[0]))) continue;
        char path[PATH_MAX_LEN], rel[PATH_MAX_LEN];
        path_join(path, sizeof path, c->dir, card);
        snprintf(rel, sizeof rel, ROOT "%s", card);
        size_t before = l->n;
        int rc = dir_exists(path) ? list_tree(l, path, rel, 0) : file_exists(path) ? list_add(l, rel) : 0;
        if (rc) return rc;
        /* sigil tells a folder card by the files listed in it: an empty folder
         * (PS5SX2 makes memcards/MemoryCard empty) gets the empty superblock
         * PCSX2 gives a new folder card (FileMcd_CreateNewCard), else the save
         * would go on the other slot's card. */
        if (l->n == before && dir_exists(path)) {
            char sb[PATH_MAX_LEN];
            path_join(sb, sizeof sb, path, "_pcsx2_superblock");
            if (write_file_atomic(sb, "", 0) != 0) continue; /* unwritable: listed as before */
            snprintf(sb, sizeof sb, "%s/_pcsx2_superblock", rel);
            if (list_add(l, sb) != 0) return -1;
        }
    }
    return 0;
}

/* The real path of a save root path sigil names; -1 outside memcards/. */
static int real_path(const ps2_cards *c, const char *rel, char *out, size_t n) {
    if (strncmp(rel, ROOT, sizeof ROOT - 1) != 0 || strstr(rel, "..")) return -1;
    path_join(out, n, c->dir, rel + sizeof ROOT - 1);
    return 0;
}

static sigil_io *open_member(void *ctx, const char *rel) {
    char path[PATH_MAX_LEN];
    return real_path(ctx, rel, path, sizeof path) == 0 ? sigil_io_open_file(path) : NULL;
}

/* A file or folder of ours the emulator writes to as well: open to it, as the
 * ones it makes are (it may not run as the payload's user). */
static void share(const char *path, int dir) { chmod(path, dir ? 0777 : 0666); }

static int write_member(void *ctx, const char *rel, const uint8_t *data, size_t len) {
    char path[PATH_MAX_LEN], parent[PATH_MAX_LEN];
    if (real_path(ctx, rel, path, sizeof path) != 0) return -1;
    str_copy(parent, sizeof parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    const int made = !dir_exists(parent);
    mkdir_parent(path);
    if (made) share(parent, 1);
    if (write_file_atomic(path, data, len) != 0) return -1;
    share(path, 0);
    return 0;
}

/* A path ending in '/' is a save folder sigil emptied. */
static int remove_member(void *ctx, const char *rel) {
    char path[PATH_MAX_LEN];
    if (real_path(ctx, rel, path, sizeof path) != 0) return -1;
    size_t l = strlen(path);
    if (l && path[l - 1] == '/') {
        path[l - 1] = 0;
        return rmdir(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

typedef struct {
    sigil_sync_request req;
    sigil_result result;
    sigil_save_option options[2];
    const char *ids[1];
    str_list listing;
} request;

static int request_init(request *q, const ps2_cards *c, const char *content_path, const char *serial) {
    memset(q, 0, sizeof *q);
    if (list_cards(c, &q->listing) != 0) return -1;
    q->result.struct_version = SIGIL_RESULT_V4;
    q->result.platform = SIGIL_PLATFORM_PS2;
    q->result.usage = SIGIL_USAGE_FOLDER_PREFIX;
    str_copy(q->result.title_id, sizeof q->result.title_id, serial);
    q->options[0] = (sigil_save_option){"Slot1_Filename", c->slot[0]};
    q->options[1] = (sigil_save_option){"Slot2_Filename", c->slot[1]};
    q->ids[0] = serial;
    sigil_sync_request *r = &q->req;
    r->struct_version = SIGIL_SYNC_REQUEST_V1;
    r->save.struct_version = SIGIL_SAVE_REQUEST_V1;
    r->save.layout = "pcsx2_standalone";
    r->save.platform = "ps2";
    r->save.content_path = content_path;
    r->save.result = &q->result;
    r->save.options = q->options;
    r->save.option_count = c->slot[1][0] ? 2 : 1;
    /* No card there yet is an empty listing, not none: sigil refuses a NULL one. */
    static const char *const no_files[1] = {NULL};
    r->save.listing = q->listing.items ? (const char *const *)q->listing.items : no_files;
    r->save.listing_count = q->listing.n;
    r->save.open = open_member;
    r->save.open_ctx = (void *)c;
    r->game_ids = q->ids;
    r->game_id_count = 1;
    r->mode = SIGIL_SYNC_UNMANAGED;
    /* sync.c settles conflicts and keeps a backup before a download. */
    r->overwrite_local = 1;
    r->write = write_member;
    r->remove = remove_member;
    r->write_ctx = (void *)c;
    return 0;
}

static void log_refusal(const char *what, const char *serial, int rc, const sigil_sync_result *r) {
    LOGW("ps2 %s %s: sigil error %d%s%s", what, serial, rc, r && r->problem[0] ? ", " : "", r ? r->problem : "");
}

int ps2card_collect(const ps2_cards *c, const char *content_path, const char *serial, unsigned char **unit,
                    size_t *len, char hash[33], char artifact[256], char identity[33]) {
    *unit = NULL;
    *len = 0;
    hash[0] = artifact[0] = identity[0] = 0;
    request q;
    sigil_sync_result *r = NULL;
    int rc = request_init(&q, c, content_path, serial) == 0 ? sigil_collect(&q.req, &r) : SIGIL_ERR_OOM;
    if (rc == SIGIL_OK && r->data) {
        *unit = r->data;
        *len = r->len;
        r->data = NULL;
        str_copy(hash, 33, r->content_hash);
        str_copy(artifact, 256, r->artifact);
        str_copy(identity, 33, r->identity_hash);
    } else if (rc != SIGIL_OK && rc != SIGIL_ERR_NOT_FOUND) {
        log_refusal("collect", serial, rc, r);
    }
    sigil_sync_result_free(r);
    list_free(&q.listing);
    return rc == SIGIL_OK || rc == SIGIL_ERR_NOT_FOUND ? 0 : -1;
}

int ps2card_restore(const ps2_cards *c, const char *content_path, const char *serial, const unsigned char *unit,
                    size_t len) {
    request q;
    sigil_sync_result *r = NULL;
    int rc = request_init(&q, c, content_path, serial) == 0 ? sigil_restore(&q.req, unit, len, &r) : SIGIL_ERR_OOM;
    if (rc != SIGIL_OK) log_refusal("restore", serial, rc, r);
    sigil_sync_result_free(r);
    list_free(&q.listing);
    return rc == SIGIL_OK ? 0 : -1;
}

int ps2card_blank(const char *path) {
    /* The card's timestamp: unused, second, minute, hour, day, month, year (LE16). */
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    const int year = tm.tm_year + 1900;
    const uint8_t tod[PS2_TOD_SIZE] = {0, (uint8_t)tm.tm_sec, (uint8_t)tm.tm_min, (uint8_t)tm.tm_hour,
                                       (uint8_t)tm.tm_mday, (uint8_t)(tm.tm_mon + 1), (uint8_t)(year & 0xFF), (uint8_t)(year >> 8)};
    sigil_ps2_card card;
    memset(&card, 0, sizeof card);
    int rc = sigil_ps2_card_format(&card, tod) == SIGIL_OK ? 0 : -1;
    const size_t len = rc == 0 ? sigil_ps2_card_file_size(&card) : 0;
    uint8_t *data = len ? malloc(len) : NULL;
    if (!data || sigil_ps2_card_write(&card, data) != SIGIL_OK) rc = -1;
    sigil_ps2_card_free(&card);
    char parent[PATH_MAX_LEN];
    str_copy(parent, sizeof parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    const int made = slash && !dir_exists(parent);
    if (rc == 0) rc = write_file_atomic(path, data, len);
    if (made) share(parent, 1);
    if (rc == 0) share(path, 0);
    free(data);
    return rc;
}

int ps2card_folder_is_game(const char *name, const char *serial) {
    char owner[SIGIL_CARD_OWNER_MAX];
    sigil_card_sony_owner(name, owner);
    return owner[0] && !strcmp(owner, serial);
}

/* Whose saves each card file holds, kept while the file stays as it is:
 * listing an 8 MB card takes a third of a second, and every sync asks it of
 * every game, in every emulator, more than once. */
#define OWNERS_CACHED 64
static struct {
    char path[1024];
    int64_t mtime_ns, size;
    char *owners; /* "\n<id>\n<id>\n" */
} g_owners[OWNERS_CACHED];
static int g_owners_next;
static pthread_mutex_t g_owners_lock = PTHREAD_MUTEX_INITIALIZER;

int ps2card_file_has(const char *path, const char *serial) {
    const int64_t mt = file_mtime_ns(path), sz = file_size(path);
    char want[64];
    snprintf(want, sizeof want, "\n%s\n", serial);
    pthread_mutex_lock(&g_owners_lock);
    for (int i = 0; i < OWNERS_CACHED; i++)
        if (g_owners[i].owners && !strcmp(g_owners[i].path, path) && g_owners[i].mtime_ns == mt && g_owners[i].size == sz) {
            const int has = strstr(g_owners[i].owners, want) != NULL;
            pthread_mutex_unlock(&g_owners_lock);
            return has;
        }
    pthread_mutex_unlock(&g_owners_lock);

    sigil_io *io = sigil_io_open_file(path);
    sigil_card_listing *l = NULL;
    size_t cap = 2, len = 1;
    char *owners = malloc(cap);
    int listed = 0;
    if (owners) strcpy(owners, "\n");
    if (owners && io && sigil_card_list(io, &l) == SIGIL_OK) {
        listed = 1;
        for (size_t i = 0; i < l->entry_count && owners; i++) {
            const size_t add = strlen(l->entries[i].owner_id) + 1;
            if (len + add + 1 > cap) {
                cap = (len + add + 1) * 2;
                char *grown = realloc(owners, cap);
                if (!grown) {
                    free(owners);
                    owners = NULL;
                    break;
                }
                owners = grown;
            }
            memcpy(owners + len, l->entries[i].owner_id, add - 1);
            owners[len + add - 1] = '\n';
            len += add;
            owners[len] = 0;
        }
    }
    sigil_card_listing_free(l);
    if (io) sigil_io_close(io);
    const int has = owners && strstr(owners, want) != NULL;
    /* Not one written a moment ago: the emulator may not be done with it,
     * and a write in the same second could leave the same time and size. */
    const int settled = time(NULL) - (time_t)(mt / 1000000000LL) > 3;
    if (owners && listed && settled && strlen(path) < sizeof g_owners[0].path) {
        pthread_mutex_lock(&g_owners_lock);
        int at = -1;
        for (int i = 0; i < OWNERS_CACHED && at < 0; i++)
            if (g_owners[i].owners && !strcmp(g_owners[i].path, path)) at = i;
        if (at < 0) at = g_owners_next++ % OWNERS_CACHED;
        free(g_owners[at].owners);
        snprintf(g_owners[at].path, sizeof g_owners[at].path, "%s", path);
        g_owners[at].mtime_ns = mt;
        g_owners[at].size = sz;
        g_owners[at].owners = owners;
        owners = NULL;
        pthread_mutex_unlock(&g_owners_lock);
    }
    free(owners);
    return has;
}

/* ---- PCSX2's settings files ---- */

static void trim(char **b, char **e) {
    while (*b < *e && isspace((unsigned char)**b)) (*b)++;
    while (*e > *b && isspace((unsigned char)(*e)[-1])) (*e)--;
}

/* Calls fn for each "key = value" line, with the [section] it's in (ini) or
 * the part of the key before its last '/' (flat), until fn returns nonzero. */
typedef int (*setting_fn)(const char *sec, size_t sl, const char *key, size_t kl, const char *val, size_t vl,
                          const char *line, const char *next, void *ctx);

static void each_setting(const char *txt, int ini, setting_fn fn, void *ctx) {
    const char *sec = "", *line = txt;
    size_t sl = 0;
    while (line && *line) {
        const char *eol = line + strcspn(line, "\n"), *next = *eol ? eol + 1 : eol;
        char *b = (char *)line, *e = (char *)eol;
        trim(&b, &e);
        if (b < e && *b == '[' && e[-1] == ']' && ini) {
            sec = b + 1;
            sl = (size_t)(e - b - 2);
        } else if (b < e && *b != '#' && *b != ';') {
            const char *eq = memchr(b, '=', (size_t)(e - b));
            if (eq) {
                char *kb = b, *ke = (char *)eq, *vb = (char *)eq + 1, *ve = e;
                trim(&kb, &ke);
                trim(&vb, &ve);
                const char *s = sec, *k = kb;
                size_t ls = sl;
                if (!ini) { /* "MemoryCards/Slot1_Filename" */
                    const char *slash = NULL;
                    for (const char *q = kb; q < ke; q++)
                        if (*q == '/') slash = q;
                    s = slash ? kb : "EmuCore/GS";
                    ls = slash ? (size_t)(slash - kb) : strlen(s);
                    k = slash ? slash + 1 : kb;
                }
                if (fn(s, ls, k, (size_t)(ke - k), vb, (size_t)(ve - vb), line, next, ctx)) return;
            }
        }
        line = next;
    }
}

/* want is a [MemoryCards] key, or "Section/Key" for one elsewhere ("PS5/CardPerGame"). */
static int is_card_key(const char *sec, size_t sl, const char *key, size_t kl, const char *want) {
    const char *slash = strchr(want, '/'), *ws = slash ? want : "MemoryCards", *wk = slash ? slash + 1 : want;
    const size_t wsl = slash ? (size_t)(slash - want) : 11;
    return sl == wsl && !strncasecmp(sec, ws, sl) && kl == strlen(wk) && !strncasecmp(key, wk, kl);
}

typedef struct {
    const char *key;
    char *out;
    size_t n;
    int found;
} get_ctx;

static int get_cb(const char *sec, size_t sl, const char *key, size_t kl, const char *val, size_t vl, const char *line,
                  const char *next, void *vctx) {
    get_ctx *c = vctx;
    (void)line, (void)next;
    if (!is_card_key(sec, sl, key, kl, c->key)) return 0;
    if (vl >= c->n) vl = c->n - 1;
    memcpy(c->out, val, vl);
    c->out[vl] = 0;
    c->found = 1;
    return 0; /* the last one wins */
}

int ps2card_setting(const char *txt, int ini, const char *key, char *out, size_t n) {
    get_ctx c = {key, out, n, 0};
    if (!txt || !n) return -1;
    out[0] = 0;
    each_setting(txt, ini, get_cb, &c);
    return c.found ? 0 : -1;
}

typedef struct {
    const char *key;
    const char *sec_line; /* ini: where [MemoryCards] starts */
    const char *spans[16][2]; /* lines of the key, to drop */
    int ns;
} set_ctx;

static int set_cb(const char *sec, size_t sl, const char *key, size_t kl, const char *val, size_t vl, const char *line,
                  const char *next, void *vctx) {
    set_ctx *c = vctx;
    (void)val, (void)vl;
    if (is_card_key(sec, sl, key, kl, c->key) && c->ns < 16) {
        c->spans[c->ns][0] = line;
        c->spans[c->ns++][1] = next;
    }
    return 0;
}

int ps2card_set_setting(const char *path, int ini, const char *key, const char *value) {
    char *old = read_file(path, NULL);
    const char *txt = old ? old : "";
    set_ctx c = {key, NULL, {{0}}, 0};
    int header_last = 0; /* [MemoryCards] is the last line, with no newline */
    each_setting(txt, ini, set_cb, &c);
    if (ini) { /* the section's header line, to add the key under */
        for (const char *line = txt; *line;) {
            const char *eol = line + strcspn(line, "\n");
            char *b = (char *)line, *e = (char *)eol;
            trim(&b, &e);
            if (e - b == 13 && !strncasecmp(b, "[MemoryCards]", 13)) {
                c.sec_line = *eol ? eol + 1 : eol;
                header_last = !*eol;
                break;
            }
            line = *eol ? eol + 1 : eol;
        }
    }
    size_t tl = strlen(txt), cap = tl + strlen(key) + strlen(value) + 64;
    char *out = malloc(cap), setting[512];
    if (!out) {
        free(old);
        return -1;
    }
    if (ini) snprintf(setting, sizeof setting, "%s = %s\n", key, value);
    else snprintf(setting, sizeof setting, "MemoryCards/%s=%s\n", key, value);
    size_t o = 0;
    for (const char *p = txt; p <= txt + tl; p++) {
        if (ini && c.sec_line && p == c.sec_line) { /* first under [MemoryCards] */
            if (header_last) out[o++] = '\n';
            memcpy(out + o, setting, strlen(setting));
            o += strlen(setting);
        }
        int skip = 0;
        for (int i = 0; i < c.ns && !skip; i++)
            if (p == c.spans[i][0]) {
                p = c.spans[i][1] - 1;
                skip = 1;
            }
        if (skip || p == txt + tl) continue;
        out[o++] = *p;
    }
    if (!ini || !c.sec_line) {
        if (o && out[o - 1] != '\n') out[o++] = '\n';
        if (ini) o += (size_t)sprintf(out + o, "%s[MemoryCards]\n", o ? "\n" : "");
        memcpy(out + o, setting, strlen(setting));
        o += strlen(setting);
    }
    char parent[PATH_MAX_LEN];
    str_copy(parent, sizeof parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    const int made = slash && !dir_exists(parent);
    int rc = write_file_atomic(path, out, o);
    if (made) share(parent, 1);
    if (rc == 0) share(path, 0);
    free(out);
    free(old);
    return rc;
}

/* ---- PCSX2's game database (GameIndex.yaml) ---- */

/* Games that read each other's saves, as memcardFilters lists them: one edge
 * per game and serial it names. Read once per file (by size and mtime). */
typedef struct {
    char a[24], b[24];
} db_edge;

static struct {
    pthread_mutex_t lock;
    char path[PATH_MAX_LEN];
    long long size, mtime;
    db_edge *edges;
    size_t n;
    char **games; /* every serial with an entry, sorted */
    size_t ngames;
} g_db = {PTHREAD_MUTEX_INITIALIZER, "", 0, 0, NULL, 0, NULL, 0};

/* A serial as PCSX2 names cards by it, upper case; 0 when it can't be one. */
static int db_key(const char *b, const char *e, char out[24]) {
    size_t n = 0;
    for (; b < e; b++) {
        if (!isalnum((unsigned char)*b) && *b != '-' && *b != '.' && *b != '_') return -1;
        if (n + 1 >= 24) return -1;
        out[n++] = (char)toupper((unsigned char)*b);
    }
    out[n] = 0;
    return n && out[0] != '.' ? 0 : -1;
}

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void db_free(void) {
    for (size_t i = 0; i < g_db.ngames; i++) free(g_db.games[i]);
    free(g_db.games);
    free(g_db.edges);
    g_db.games = NULL;
    g_db.edges = NULL;
    g_db.n = g_db.ngames = 0;
}

/* Top level "SLUS-20552:" lines are games; "  memcardFilters:" starts a list
 * of '    - "SLUS-20552"' lines (quotes and "# comments" optional). */
static int db_load(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!strcmp(g_db.path, path) && g_db.size == (long long)st.st_size && g_db.mtime == (long long)st.st_mtime) return 0;
    char *txt = read_file(path, NULL);
    if (!txt) return -1;
    db_free();
    str_copy(g_db.path, sizeof g_db.path, path);
    g_db.size = (long long)st.st_size;
    g_db.mtime = (long long)st.st_mtime;
    size_t cap = 0, gcap = 0;
    char game[24] = "";
    int in_filters = 0;
    for (char *line = txt; *line;) {
        char *eol = line + strcspn(line, "\n"), *next = *eol ? eol + 1 : eol;
        char *b = line, *e = eol;
        trim(&b, &e);
        if (line[0] != ' ' && line[0] != '\t' && line[0] != '#' && e > b && e[-1] == ':') {
            in_filters = 0;
            if (db_key(line, e - 1, game) == 0) {
                if (g_db.ngames == gcap) {
                    gcap = gcap ? gcap * 2 : 4096;
                    char **grown = realloc(g_db.games, gcap * sizeof *grown);
                    if (!grown) break;
                    g_db.games = grown;
                }
                if (!(g_db.games[g_db.ngames] = strdup(game))) break;
                g_db.ngames++;
            } else {
                game[0] = 0;
            }
        } else if (b < e && !strncmp(b, "memcardFilters:", 15)) {
            in_filters = game[0] != 0;
        } else if (in_filters && b < e && *b == '-') {
            char *v = b + 1, *ve = memchr(v, '#', (size_t)(e - v));
            if (!ve) ve = e;
            trim(&v, &ve);
            if (ve - v >= 2 && (*v == '"' || *v == '\'') && ve[-1] == *v) v++, ve--;
            db_edge ed;
            str_copy(ed.a, sizeof ed.a, game);
            if (db_key(v, ve, ed.b) == 0 && strcmp(ed.a, ed.b) != 0) {
                if (g_db.n == cap) {
                    cap = cap ? cap * 2 : 1024;
                    db_edge *grown = realloc(g_db.edges, cap * sizeof *grown);
                    if (!grown) break;
                    g_db.edges = grown;
                }
                g_db.edges[g_db.n++] = ed;
            }
        } else if (b < e && *b != '#' && (line[0] != ' ' || strspn(line, " ") <= 2)) {
            in_filters = 0; /* the game's next key */
        }
        line = next;
    }
    free(txt);
    qsort(g_db.games, g_db.ngames, sizeof *g_db.games, str_cmp);
    return 0;
}

int ps2card_db_card(const char *db, const char *serial, char *out, size_t n) {
    char key[24];
    out[0] = 0;
    if (!db || !db[0] || !serial || db_key(serial, serial + strlen(serial), key) != 0) return -1;
    pthread_mutex_lock(&g_db.lock);
    if (db_load(db) != 0) {
        pthread_mutex_unlock(&g_db.lock);
        return -1;
    }
    const char *k = key;
    int rc = -1;
    if (bsearch(&k, g_db.games, g_db.ngames, sizeof *g_db.games, str_cmp)) {
        /* The game's group, grown until nothing joins it: its smallest serial
         * names the card. A few hundred edges, so a plain sweep does. */
        str_list group = {0};
        list_add(&group, key);
        for (int grew = 1; grew;) {
            grew = 0;
            for (size_t i = 0; i < g_db.n; i++) {
                int ha = 0, hb = 0;
                for (size_t j = 0; j < group.n; j++) {
                    ha |= !strcmp(group.items[j], g_db.edges[i].a);
                    hb |= !strcmp(group.items[j], g_db.edges[i].b);
                }
                if (ha != hb && list_add(&group, ha ? g_db.edges[i].b : g_db.edges[i].a) == 0) grew = 1;
            }
        }
        if (group.n) { /* empty only when memory ran out */
            const char *least = group.items[0];
            for (size_t j = 1; j < group.n; j++)
                if (strcmp(group.items[j], least) < 0) least = group.items[j];
            snprintf(out, n, "%s.ps2", least);
            rc = 0;
        }
        list_free(&group);
    }
    pthread_mutex_unlock(&g_db.lock);
    return rc;
}
