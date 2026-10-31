#include "state.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "util.h"

static cJSON *g_state;
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
/* For state_index: it changes when another thread takes the lock, or a record
 * is removed or moved, so an index built before knows it may be out of date. */
static pthread_t g_lock_holder;
static int g_lock_held_once;
static unsigned long g_lock_epoch;

void state_lock(void) {
    pthread_mutex_lock(&g_state_lock);
    const pthread_t self = pthread_self();
    if (!g_lock_held_once || !pthread_equal(self, g_lock_holder)) {
        g_lock_holder = self;
        g_lock_held_once = 1;
        g_lock_epoch++;
    }
}
void state_unlock(void) { pthread_mutex_unlock(&g_state_lock); }

static void state_path(char *out, size_t n) { path_join(out, n, plat_data_dir(), "state.json"); }

int state_load(void) {
    char path[PATH_MAX_LEN];
    state_path(path, sizeof path);
    char *txt = read_file(path, NULL);
    cJSON_Delete(g_state);
    g_state = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (!cJSON_IsObject(g_state)) {
        if (txt) LOGW("state.json unreadable, starting fresh");
        cJSON_Delete(g_state);
        g_state = cJSON_CreateObject();
    }
    g_lock_epoch++;
    return 0;
}

int state_save(void) {
    char path[PATH_MAX_LEN];
    state_path(path, sizeof path);
    char *txt = cJSON_PrintUnformatted(g_state);
    int rc = txt ? write_file_atomic(path, txt, strlen(txt)) : -1;
    free(txt);
    if (rc != 0) LOGE("failed to write %s", path);
    return rc;
}

cJSON *state_section(const char *name) {
    cJSON *s = cJSON_GetObjectItemCaseSensitive(g_state, name);
    if (!s) {
        s = strcmp(name, "history") == 0 ? cJSON_CreateArray() : cJSON_CreateObject();
        cJSON_AddItemToObject(g_state, name, s);
    }
    return s;
}

cJSON *state_entry(const char *section, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(state_section(section), key);
}

cJSON *state_entry_ensure(const char *section, const char *key) {
    cJSON *sec = state_section(section);
    cJSON *e = cJSON_GetObjectItemCaseSensitive(sec, key);
    if (!e) {
        e = cJSON_CreateObject();
        cJSON_AddItemToObject(sec, key, e);
    }
    return e;
}

void state_entry_remove(const char *section, const char *key) {
    cJSON_DeleteItemFromObjectCaseSensitive(state_section(section), key);
    g_lock_epoch++;
}

struct state_index {
    char section[32];
    int built;
    unsigned long epoch; /* g_lock_epoch when built */
    cJSON **slots;
    size_t cap; /* a power of two, at least twice the records; 0: no memory */
};

static void index_build(state_index *ix) {
    const cJSON *sec = state_section(ix->section);
    size_t n = 0, cap = 64;
    for (const cJSON *e = sec->child; e; e = e->next) n++;
    while (cap < 2 * n) cap *= 2;
    free(ix->slots);
    ix->slots = calloc(cap, sizeof *ix->slots);
    ix->cap = ix->slots ? cap : 0;
    for (cJSON *e = sec->child; e && ix->cap; e = e->next) {
        if (!e->string) continue;
        size_t i = str_hash(e->string) & (cap - 1);
        while (ix->slots[i] && strcmp(ix->slots[i]->string, e->string)) i = (i + 1) & (cap - 1);
        if (!ix->slots[i]) ix->slots[i] = e; /* a repeated key: the first, as cJSON finds */
    }
    ix->built = 1;
    ix->epoch = g_lock_epoch;
}

state_index *state_index_new(const char *section) {
    state_index *ix = calloc(1, sizeof *ix);
    if (ix) str_copy(ix->section, sizeof ix->section, section);
    return ix;
}

cJSON *state_index_get(state_index *ix, const char *key) {
    if (!ix->built || ix->epoch != g_lock_epoch) index_build(ix);
    for (size_t i = ix->cap ? str_hash(key) & (ix->cap - 1) : 0; ix->cap && ix->slots[i]; i = (i + 1) & (ix->cap - 1))
        if (!strcmp(ix->slots[i]->string, key)) return ix->slots[i];
    return state_entry(ix->section, key);
}

void state_index_free(state_index *ix) {
    if (!ix) return;
    free(ix->slots);
    free(ix);
}

/* moved: an emulator's own folder move. A record already at the new name is
 * newer (written since the move) and stays; one whose file is still at the
 * old name only stays there, with its file. */
static int rekey(const char *from, const char *to, int moved) {
    static const char *const sections[] = {"roms", "saves", "states", "conflicts"};
    size_t fl = strlen(from);
    int n = 0;
    g_lock_epoch++;
    for (size_t s = 0; s < sizeof sections / sizeof *sections; s++) {
        cJSON *sec = state_section(sections[s]), *found = cJSON_CreateArray(), *e = sec->child;
        while (e) {
            cJSON *next = e->next;
            if (!strncmp(e->string, from, fl) && (!e->string[fl] || e->string[fl] == '/' || e->string[fl] == '#'))
                cJSON_AddItemToArray(found, cJSON_DetachItemViaPointer(sec, e));
            e = next;
        }
        while ((e = cJSON_DetachItemFromArray(found, 0))) {
            char key[PATH_MAX_LEN + 256], was[PATH_MAX_LEN + 256], now[PATH_MAX_LEN + 256];
            snprintf(key, sizeof key, "%s%s", to, e->string + fl);
            if (moved) {
                /* A bundle's record is "<folder>#<game>": its folder's what moved. */
                snprintf(was, sizeof was, "%s", e->string);
                snprintf(now, sizeof now, "%s", key);
                was[strcspn(was, "#")] = now[strcspn(now, "#")] = 0;
                const int there = file_exists(now) || dir_exists(now), left = file_exists(was) || dir_exists(was);
                if (cJSON_GetObjectItemCaseSensitive(sec, key) || (left && !there)) {
                    cJSON_AddItemToObject(sec, e->string, e); /* (back where it was) */
                    continue;
                }
            }
            cJSON_DeleteItemFromObjectCaseSensitive(sec, key); /* an old record of something gone */
            cJSON_AddItemToObject(sec, key, e);
            n++;
        }
        cJSON_Delete(found);
    }
    return n;
}

int state_rekey(const char *from, const char *to) { return rekey(from, to, 0); }

int state_rekey_moved(const char *from, const char *to) { return rekey(from, to, 1); }

void jset_num(cJSON *o, const char *k, double v) {
    cJSON *n = cJSON_CreateNumber(v);
    if (cJSON_GetObjectItemCaseSensitive(o, k))
        cJSON_ReplaceItemInObjectCaseSensitive(o, k, n);
    else
        cJSON_AddItemToObject(o, k, n);
}

void jset_str(cJSON *o, const char *k, const char *v) {
    cJSON *s = cJSON_CreateString(v ? v : "");
    if (cJSON_GetObjectItemCaseSensitive(o, k))
        cJSON_ReplaceItemInObjectCaseSensitive(o, k, s);
    else
        cJSON_AddItemToObject(o, k, s);
}

double jget_num(const cJSON *o, const char *k, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

const char *jget_str(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

void state_history_add(cJSON *summary) {
    cJSON *h = state_section("history");
    cJSON_AddItemToArray(h, summary);
    while (cJSON_GetArraySize(h) > 30) cJSON_DeleteItemFromArray(h, 0);
}
