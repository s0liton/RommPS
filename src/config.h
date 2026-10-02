/* config.json */
#ifndef ROMM_CONFIG_H
#define ROMM_CONFIG_H

#include "cJSON.h"

typedef enum { CONFLICT_ASK, CONFLICT_NEWEST, CONFLICT_LOCAL, CONFLICT_SERVER } conflict_policy;

typedef struct {
    char server_url[512];     /* https://romm.example.com (no trailing slash) */
    char token[256];          /* rmm_... client API token */
    char device_id[64];       /* RomM device UUID */
    char device_name[64];
    char client_device_identifier[40]; /* sent when pairing */
    char username[64];
    int tls_verify;
    int web_port;

    int sync_interval_min;    /* periodic sync, 0 = off */
    int sync_on_game_exit;
    int sync_on_game_start;
    int sync_on_change;       /* upload saves as soon as they change */
    int exit_delay_sec;
    int sync_states;          /* 0 off, 1 upload, 2 upload and download */
    int notify;
    int keep_backups;         /* local copies kept per save */
    int server_versions;      /* versions RomM keeps per save, 0 keeps all */
    int download_concurrency; /* library downloads running at once */
    conflict_policy conflicts;
    char slot[32];            /* RomM save slot */

    cJSON *profiles;
    int profiles_custom;      /* 0: built-in profiles, not written to config.json */
    int setup_complete;       /* background syncs wait for the setup wizard */
    char ps2_cards[16];       /* "per_game" or "backup" */
    char server_version[32];
} config_t;

extern config_t g_cfg;

void config_lock(void);
void config_unlock(void);

int config_load(void);  /* loads or creates defaults */
int config_save(void);

/* The config as JSON, with or without the token. */
cJSON *config_to_json(int include_secrets);
/* Applies the settings the UI is allowed to change. */
void config_apply_json(const cJSON *j);

const char *conflict_policy_name(conflict_policy p);
/* The paired server is at least this version. */
int config_server_at_least(int major, int minor, int patch);
int config_is_paired(void);

#endif
