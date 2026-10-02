/* Emulator detection for the setup wizard. */
#ifndef ROMM_DETECT_H
#define ROMM_DETECT_H

#include "cJSON.h"

/* Emulators found on the console, each with a ready-made profile. */
cJSON *detect_emulators(void);

/* Rebuilds profiles an older version of setup generated, so they pick up new
 * features. Profiles written by hand are left alone. Returns how many changed. */
int detect_refresh_profiles(cJSON *profiles);

#endif
