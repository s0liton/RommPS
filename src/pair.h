/* Pairing with a RomM server. */
#ifndef ROMM_PAIR_H
#define ROMM_PAIR_H

#include "cJSON.h"

/* Shows a code to approve in RomM. */
int pair_start(const char *server_url, char *err, int en);
/* Uses a pairing code from RomM's Client API Tokens page. */
int pair_with_code(const char *server_url, const char *code, char *err, int en);
void pair_forget(void);
cJSON *pair_status(void);

#endif
