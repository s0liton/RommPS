#include "state.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "util.h"

static cJSON *g_state;
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;

void state_lock(void) { pthread_mutex_lock(&g_state_lock); }
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
}

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
