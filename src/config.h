/* config.json */
#ifndef ROMM_CONFIG_H
#define ROMM_CONFIG_H

#include "cJSON.h"

typedef enum { CONFLICT_ASK, CONFLICT_NEWEST, CONFLICT_LOCAL, CONFLICT_SERVER } conflict_policy;

typedef struct {
    char server_url[512];     /* https://romm.example.com (no trailing slash) */
    char token[256];          /* rmm_... client API token */
    char device_id[64];       /* RomM device UUID */
    char device_name[64];     /* what this console is called on RomM and in names it uploads */
    int device_name_auto;     /* device_name follows the console's own name (saved as "") */
    char romm_device_name[64]; /* the name RomM has for the device, last we told it */
    char client_device_identifier[40]; /* sent when pairing */
    char username[64];
    int tls_verify;
    char client_cert[256];          /* mTLS .p12, relative to the data folder (or absolute); "" for none */
    char client_cert_password[256]; /* its password, kept like the token */
    int web_port;

    int sync_interval_min;    /* periodic sync, 0 = off */
    int sync_on_game_exit;
    int sync_on_game_start;
    int save_sync;            /* 0: RomM only as a game source, no save or state sync */
    int sync_on_change;       /* upload saves as soon as they change */
    int exit_delay_sec;
    int sync_states;          /* 0 off, 1 upload, 2 upload and download */
    int notify;
    int update_check;         /* look for new releases once a day */
    int keep_backups;         /* local copies kept per save */
    int server_versions;      /* versions RomM keeps per save, 0 keeps all */
    int download_concurrency; /* library downloads running at once */
    conflict_policy conflicts;
    char slot[32];            /* RomM save slot */

    cJSON *profiles;
    int profiles_custom;      /* 0: built-in profiles, not written to config.json */
    int setup_complete;       /* background syncs wait for the setup wizard */
    char ps2_cards[16];       /* PS2's memory card method: "per_game", "shared" or "" (as its emulator keeps them) */
    int ps2_cards_checked;    /* memcard_keep_shared ran: from 1.4.0, one card per game is the default */
    char ui_theme[24];        /* web UI theme id (ui/themes/<id>.css) */
    int cover_cache;          /* keep every game's cover on the console (covers.h) */
    char server_version[32];
} config_t;

extern config_t g_cfg;

void config_lock(void);
void config_unlock(void);

int config_load(void);  /* loads or creates defaults */
/* The memory card method chosen for a system ("per_game", "shared"), or ""
 * for the one its main emulator keeps (profiles.c). Caller holds the lock. */
const char *config_card_mode(const char *system);
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
