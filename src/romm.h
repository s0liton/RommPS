/* RomM API client. Needs RomM 5.2.0 or newer. */
#ifndef ROMM_API_H
#define ROMM_API_H

#include <stdint.h>

#include "cJSON.h"
#include "http.h"

/* Scopes requested when pairing. */
#define ROMM_SCOPES                                                                           \
    "me.read", "platforms.read", "roms.read", "roms.user.read", "firmware.read", "assets.read", \
        "assets.write", "devices.read", "devices.write"

typedef struct {
    char device_code[128];
    char user_code[32];
    char verification_url[640];
    int interval;
    int expires_in;
} romm_device_flow;

typedef enum { POLL_OK, POLL_PENDING, POLL_SLOW_DOWN, POLL_DENIED, POLL_EXPIRED, POLL_ERROR } romm_poll;

/* JSON request to the paired server. Returns the parsed body (free it) or NULL. */
cJSON *romm_call(const char *method, const char *path, const cJSON *body, long *status);
/* Same, with an explicit server and auth, for use before pairing. */
cJSON *romm_call_raw(const char *base, const char *auth, const char *method, const char *path,
                     const cJSON *body, long *status);

/* Checks the server and fills in its version. 0 on success. */
int romm_heartbeat(const char *base, char *version, int vn);

/* Device pairing: the user approves a code in RomM. */
int romm_device_init(const char *base, romm_device_flow *flow);
romm_poll romm_device_poll(const char *base, const char *device_code, char *token, int tn,
                           char *device_id, int dn);
/* Pairing with a code from RomM's Client API Tokens page. */
int romm_pair_exchange(const char *base, const char *code, char *token, int tn);
int romm_register_device(const char *base, const char *token, char *device_id, int dn);

/* Downloads a server path to dest, resuming if possible. */
int romm_download(const char *path, const char *dest, http_progress_fn cb, void *ud,
                  http_resp *resp);
/* Uploads one file (POST or PUT). Returns the parsed response. */
cJSON *romm_upload(const char *method, const char *path, const char *field, const char *file,
                   const char *upload_name, long *status);

/* The "detail" message from a RomM error response. */
void romm_error_detail(const cJSON *j, char *out, int n);

#endif
