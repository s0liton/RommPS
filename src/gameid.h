/* Game ids read from disc images, used to find a game's save files. */
#ifndef ROMM_GAMEID_H
#define ROMM_GAMEID_H

#include <stddef.h>

/* PSP disc id ("ULUS10041") from an .iso or .cso. */
int gameid_psp(const char *path, char *out, size_t n);

/* GameCube or Wii game id ("GALE01") from .iso, .gcm, .rvz, .wia, .ciso or .wbfs. */
int gameid_dolphin(const char *path, char *out, size_t n);

/* A string value from a PARAM.SFO file, e.g. "TITLE" or "DISC_ID". */
int sfo_get(const unsigned char *sfo, size_t len, const char *key, char *out, size_t n);
int sfo_file_get(const char *path, const char *key, char *out, size_t n);

#endif
