/* mTLS: the client certificate (a PKCS#12 .p12 file) shown to the RomM
 * server, for servers behind a proxy that asks for one (Traefik, Pangolin,
 * nginx). config.json's client_cert names the file, relative to the data
 * folder (/data/romm-sync) or absolute; client_cert_password is its password. */
#ifndef ROMM_CLIENTCERT_H
#define ROMM_CLIENTCERT_H

#include <stddef.h>

#include "cJSON.h"

/* Loads the configured certificate, if any (at start, after http_global_init). */
void clientcert_init(void);

/* Takes a .p12 file sent from the web UI (base64), checks it opens with the
 * password, saves it as client.p12 in the data folder and uses it from now
 * on. 0, or -1 and why in err. */
int clientcert_install(const char *p12_base64, const char *password, char *err, size_t en);

/* Stops using the certificate and forgets it (client.p12 is deleted). */
void clientcert_remove(void);

/* {file, active, info, error, refused} */
cJSON *clientcert_status(void);

#endif
