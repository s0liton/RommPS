/* Game ids read from disc images, used to find a game's save files. */
#ifndef ROMM_GAMEID_H
#define ROMM_GAMEID_H

#include <stddef.h>
#include <stdint.h>

/* PSP disc id ("ULUS10041") from an .iso or .cso. */
int gameid_psp(const char *path, char *out, size_t n);

/* GameCube or Wii game id ("GALE01") from .iso, .gcm, .rvz, .wia, .ciso or .wbfs. */
int gameid_dolphin(const char *path, char *out, size_t n);

/* 3DS title id ("0004000000066C00") from a .cci/.3ds (NCSD), .cxi/.app
 * (NCCH) or .cia header: plain text even in encrypted dumps. */
int gameid_3ds(const char *path, char *out, size_t n);

/* The title id an emulator front end has for a game file: PS5CEMU-HAR's
 * library.json ({"wiiu": [{"titleId", "path"}], "3ds": [...]}), the only way to
 * a .wux's without its disc key. */
int gameid_from_library(const char *library_json, const char *system, const char *path, char *out, size_t n);

/* Dreamcast product number ("MK-51035") from the disc's IP.BIN: .gdi, .cue,
 * .chd or a one-track image (not .cdi). */
int gameid_dreamcast(const char *path, char *out, size_t n);

/* The id a front end keeps for a game file in a "<path>\t<id>" per line list
 * (PSFlyCast's data/game-ids.txt), /app0/ standing for its app_root. */
int gameid_from_list(const char *list, const char *app_root, const char *path, char *out, size_t n);

/* PlayStation serial ("SCUS-94194") from SYSTEM.CNF: .iso, .bin, .cue or .chd. */
int gameid_psx(const char *path, char *out, size_t n);

/* A PS2 game's CRC as PCSX2 names its per-game settings by ("SLUS-20552_248E6126.ini"):
 * every 32-bit word of the boot program SYSTEM.CNF's BOOT2 names, XORed. .iso,
 * .bin, .cue or .chd (CD or DVD). */
int gameid_ps2_crc(const char *path, uint32_t *crc);

/* An Xbox game's title id as its save folder is named, E:\UDATA\4D530004:
 * the XBE certificate's, from a .xiso or full disc image, a default.xbe or the
 * game's folder. */
int gameid_xbox(const char *path, char *out, size_t n);

/* An Xbox disc image cut short: its root directory has files ending past the
 * end of the file (a copy that stopped part way). 1 then, with the bytes the
 * image needs at least and the bytes it has; 0 for a whole one or no image. */
int gameid_xbox_short(const char *path, uint64_t *need, uint64_t *have);

/* An Xbox 360 game's title id, "4D5307E6", as its save folder is named: the
 * XEX's execution info, from a disc image (.iso, .xiso), a default.xex or the
 * game's folder; a GOD folder or an STFS package (Xbox Live Arcade), from its
 * header. */
int gameid_x360(const char *path, char *out, size_t n);

/* A string value from a PARAM.SFO file, e.g. "TITLE" or "DISC_ID". */
int sfo_get(const unsigned char *sfo, size_t len, const char *key, char *out, size_t n);
int sfo_file_get(const char *path, const char *key, char *out, size_t n);

#endif
