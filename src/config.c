#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "profiles.h"
#include "util.h"

config_t g_cfg;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

void config_lock(void) { pthread_mutex_lock(&g_cfg_lock); }
void config_unlock(void) { pthread_mutex_unlock(&g_cfg_lock); }

static void config_path(char *out, size_t n) { path_join(out, n, plat_data_dir(), "config.json"); }

const char *conflict_policy_name(conflict_policy p) {
    switch (p) {
    case CONFLICT_NEWEST: return "newest";
    case CONFLICT_LOCAL: return "local";
    case CONFLICT_SERVER: return "server";
    default: return "ask";
    }
}

static conflict_policy parse_policy(const char *s) {
    if (!s) return CONFLICT_ASK;
    if (!strcmp(s, "newest")) return CONFLICT_NEWEST;
    if (!strcmp(s, "local")) return CONFLICT_LOCAL;
    if (!strcmp(s, "server")) return CONFLICT_SERVER;
    return CONFLICT_ASK;
}

static void get_str(const cJSON *j, const char *key, char *dst, size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (cJSON_IsString(v)) str_copy(dst, n, v->valuestring);
}

static void get_int(const cJSON *j, const char *key, int *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (cJSON_IsNumber(v)) *dst = v->valueint;
    else if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}

static void set_defaults(void) {
    memset(&g_cfg, 0, sizeof g_cfg);
    str_copy(g_cfg.device_name, sizeof g_cfg.device_name, "PlayStation 5");
    g_cfg.tls_verify = 1;
    g_cfg.web_port = 8780;
    g_cfg.sync_interval_min = 15;
    g_cfg.sync_on_game_exit = 1;
    g_cfg.sync_on_game_start = 1;
    g_cfg.sync_on_change = 1;
    g_cfg.exit_delay_sec = 5;
    g_cfg.sync_states = 1;
    g_cfg.notify = 1;
    g_cfg.update_check = 1;
    g_cfg.keep_backups = 5;
    g_cfg.server_versions = 10;
    g_cfg.download_concurrency = 3;
    g_cfg.conflicts = CONFLICT_ASK;
    str_copy(g_cfg.slot, sizeof g_cfg.slot, "autosave");
    str_copy(g_cfg.ps2_cards, sizeof g_cfg.ps2_cards, "per_game");
}

void config_apply_json(const cJSON *j) {
    char pol[16] = "";
    get_str(j, "server_url", g_cfg.server_url, sizeof g_cfg.server_url);
    size_t l = strlen(g_cfg.server_url);
    while (l && g_cfg.server_url[l - 1] == '/') g_cfg.server_url[--l] = 0;
    get_str(j, "device_name", g_cfg.device_name, sizeof g_cfg.device_name);
    get_int(j, "tls_verify", &g_cfg.tls_verify);
    get_int(j, "web_port", &g_cfg.web_port);
    get_int(j, "sync_interval_min", &g_cfg.sync_interval_min);
    get_int(j, "sync_on_game_exit", &g_cfg.sync_on_game_exit);
    get_int(j, "sync_on_game_start", &g_cfg.sync_on_game_start);
    get_int(j, "sync_on_change", &g_cfg.sync_on_change);
    get_int(j, "exit_delay_sec", &g_cfg.exit_delay_sec);
    get_int(j, "sync_states", &g_cfg.sync_states); /* older configs: true/false */
    char states[16] = "";
    get_str(j, "states", states, sizeof states);
    if (!strcmp(states, "off")) g_cfg.sync_states = 0;
    else if (!strcmp(states, "upload")) g_cfg.sync_states = 1;
    else if (!strcmp(states, "sync")) g_cfg.sync_states = 2;
    get_int(j, "notify", &g_cfg.notify);
    get_int(j, "update_check", &g_cfg.update_check);
    get_int(j, "keep_backups", &g_cfg.keep_backups);
    get_int(j, "server_versions", &g_cfg.server_versions);
    if (g_cfg.server_versions < 0) g_cfg.server_versions = 0;
    if (g_cfg.server_versions > 100) g_cfg.server_versions = 100;
    get_int(j, "download_concurrency", &g_cfg.download_concurrency);
    if (g_cfg.download_concurrency < 1) g_cfg.download_concurrency = 1;
    if (g_cfg.download_concurrency > 8) g_cfg.download_concurrency = 8;
    get_str(j, "slot", g_cfg.slot, sizeof g_cfg.slot);
    get_str(j, "ps2_cards", g_cfg.ps2_cards, sizeof g_cfg.ps2_cards);
    if (strcmp(g_cfg.ps2_cards, "backup") != 0) str_copy(g_cfg.ps2_cards, sizeof g_cfg.ps2_cards, "per_game");
    get_str(j, "conflict_policy", pol, sizeof pol);
    if (pol[0]) g_cfg.conflicts = parse_policy(pol);
    if (!g_cfg.slot[0]) str_copy(g_cfg.slot, sizeof g_cfg.slot, "autosave");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "profiles");
    if (cJSON_IsArray(p)) {
        cJSON_Delete(g_cfg.profiles);
        g_cfg.profiles = cJSON_Duplicate(p, 1);
        g_cfg.profiles_custom = 1;
    }
    const cJSON *reset = cJSON_GetObjectItemCaseSensitive(j, "reset_profiles");
    if (cJSON_IsTrue(reset)) {
        cJSON_Delete(g_cfg.profiles);
        g_cfg.profiles = profiles_default();
        g_cfg.profiles_custom = 0;
    }
}

int config_load(void) {
    char path[PATH_MAX_LEN];
    set_defaults();
    config_path(path, sizeof path);
    char *txt = read_file(path, NULL);
    if (txt) {
        cJSON *j = cJSON_Parse(txt);
        free(txt);
        if (j) {
            config_apply_json(j);
            get_str(j, "token", g_cfg.token, sizeof g_cfg.token);
            get_str(j, "device_id", g_cfg.device_id, sizeof g_cfg.device_id);
            get_str(j, "username", g_cfg.username, sizeof g_cfg.username);
            get_str(j, "server_version", g_cfg.server_version, sizeof g_cfg.server_version);
            get_str(j, "client_device_identifier", g_cfg.client_device_identifier,
                    sizeof g_cfg.client_device_identifier);
            /* Installs paired before the wizard existed are already set up. */
            const cJSON *sc = cJSON_GetObjectItemCaseSensitive(j, "setup_complete");
            g_cfg.setup_complete = sc ? cJSON_IsTrue(sc) : (g_cfg.token[0] && g_cfg.device_id[0]);
            /* Older versions saved the default profiles; only keep edited ones. */
            const cJSON *pc = cJSON_GetObjectItemCaseSensitive(j, "profiles_custom");
            if (!cJSON_IsTrue(pc) && g_cfg.profiles) {
                cJSON_Delete(g_cfg.profiles);
                g_cfg.profiles = NULL;
                g_cfg.profiles_custom = 0;
            }
            cJSON_Delete(j);
        } else {
            LOGE("config.json is not valid JSON; using defaults");
        }
    }
    if (!g_cfg.profiles) g_cfg.profiles = profiles_default();
    if (!g_cfg.client_device_identifier[0]) random_uuid(g_cfg.client_device_identifier);
    return config_save();
}

cJSON *config_to_json(int include_secrets) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "server_url", g_cfg.server_url);
    cJSON_AddStringToObject(j, "device_name", g_cfg.device_name);
    cJSON_AddStringToObject(j, "device_id", g_cfg.device_id);
    cJSON_AddStringToObject(j, "username", g_cfg.username);
    cJSON_AddStringToObject(j, "client_device_identifier", g_cfg.client_device_identifier);
    if (include_secrets) cJSON_AddStringToObject(j, "token", g_cfg.token);
    cJSON_AddBoolToObject(j, "tls_verify", g_cfg.tls_verify);
    cJSON_AddNumberToObject(j, "web_port", g_cfg.web_port);
    cJSON_AddNumberToObject(j, "sync_interval_min", g_cfg.sync_interval_min);
    cJSON_AddBoolToObject(j, "sync_on_game_exit", g_cfg.sync_on_game_exit);
    cJSON_AddBoolToObject(j, "sync_on_game_start", g_cfg.sync_on_game_start);
    cJSON_AddBoolToObject(j, "sync_on_change", g_cfg.sync_on_change);
    cJSON_AddNumberToObject(j, "exit_delay_sec", g_cfg.exit_delay_sec);
    cJSON_AddStringToObject(j, "states", g_cfg.sync_states == 2 ? "sync" : g_cfg.sync_states ? "upload" : "off");
    cJSON_AddBoolToObject(j, "notify", g_cfg.notify);
    cJSON_AddBoolToObject(j, "update_check", g_cfg.update_check);
    cJSON_AddNumberToObject(j, "keep_backups", g_cfg.keep_backups);
    cJSON_AddNumberToObject(j, "server_versions", g_cfg.server_versions);
    cJSON_AddNumberToObject(j, "download_concurrency", g_cfg.download_concurrency);
    cJSON_AddStringToObject(j, "conflict_policy", conflict_policy_name(g_cfg.conflicts));
    cJSON_AddStringToObject(j, "slot", g_cfg.slot);
    cJSON_AddStringToObject(j, "ps2_cards", g_cfg.ps2_cards);
    cJSON_AddStringToObject(j, "server_version", g_cfg.server_version);
    cJSON_AddBoolToObject(j, "profiles_custom", g_cfg.profiles_custom);
    cJSON_AddBoolToObject(j, "setup_complete", g_cfg.setup_complete);
    if (g_cfg.profiles_custom || !include_secrets)
        cJSON_AddItemToObject(j, "profiles", cJSON_Duplicate(g_cfg.profiles, 1));
    return j;
}

int config_save(void) {
    char path[PATH_MAX_LEN];
    config_path(path, sizeof path);
    cJSON *j = config_to_json(1);
    char *txt = cJSON_Print(j);
    cJSON_Delete(j);
    int rc = txt ? write_file_atomic(path, txt, strlen(txt)) : -1;
    free(txt);
    if (rc != 0) LOGE("failed to write %s", path);
    return rc;
}

int config_is_paired(void) { return g_cfg.server_url[0] && g_cfg.token[0] && g_cfg.device_id[0]; }

int config_server_at_least(int major, int minor, int patch) {
    int a = 0, b = 0, c = 0;
    config_lock();
    sscanf(g_cfg.server_version, "%d.%d.%d", &a, &b, &c);
    config_unlock();
    if (a != major) return a > major;
    if (b != minor) return b > minor;
    return c >= patch;
}
