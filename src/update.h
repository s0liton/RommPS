/* Updating the payload from GitHub releases, when the user asks for it. */
#ifndef ROMM_UPDATE_H
#define ROMM_UPDATE_H

#include "cJSON.h"

/* Looks for a newer release in the background. notify shows a toast if one is found.
 * -1 if a check or install is already running. */
int update_check(int notify);

/* The daily check, if turned on. Call it from the main loop. */
void update_tick(void);

/* Downloads, verifies and starts the newer release in the background. */
int update_install(char *err, int en);

cJSON *update_status(void);

#endif
