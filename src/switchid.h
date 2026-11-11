/* Switch title ids read from inside game files, for the dumps whose names
 * don't carry one ("New Pokemon Snap (World) (En,Ja,Fr,De,Es,It,Zh,Ko).xci",
 * No-Intro style). The save folder Eden makes for a game is named by it.
 *
 * An .nsp or .nsz is a PFS0. A ticket in it is named by its rights id, the
 * title id's 16 hex digits first: no keys needed. Without one (every .xci,
 * plenty of .nsp), the NCAs' headers name it, encrypted with prod.keys'
 * header_key (AES-128-XTS). An .xci keeps its NCAs in the secure partition
 * (root HFS0, then "secure"); an .ncz (in .nsz and .xcz) keeps the NCA's
 * first 0x4000 bytes as they were, header included. Only headers are read:
 * an .xci runs to many GB. */
#ifndef ROMM_SWITCHID_H
#define ROMM_SWITCHID_H

#include <stddef.h>
#include <stdint.h>

/* The base game's title id, "0100F4300BF2C000", from an .nsp, .nsz, .xci,
 * .xcz, .nca or .ncz (by what's in it, not the extension), or a folder of
 * them. An update's names its game (the last three digits 000); DLC's
 * doesn't count. header_key: prod.keys' (32 bytes), NULL for none.
 * 0 when found, -1 when not; -2 when an NCA header might have said, given
 * the key. */
int switchid_from_file(const char *path, const unsigned char *header_key, char *out, size_t n);

/* What a title id is, by its last digits: a game's ends in 000 (a second
 * program's in 001 on), its update's in 800, its DLC's past the game's 1000. */
typedef enum { SWITCHID_NONE, SWITCHID_GAME, SWITCHID_UPDATE, SWITCHID_DLC } switchid_kind;
switchid_kind switchid_kind_of(uint64_t id);
/* The base game's id for any of its titles: 0100F4300BF2C000 for its update
 * 0100F4300BF2C800 and its DLC 0100F4300BF2D001. */
uint64_t switchid_game_of(uint64_t id);

/* The title id a file is, as it is, where switchid_from_file only names the
 * game: an update's own (ending in 800), DLC's (1001 on), so "Some Game
 * v3.0.3.nsp" can be told for the update it is. A file (or folder) holding
 * the game is the game, whatever rides along with it; an update beats DLC.
 * Tickets first, then NCA headers, which take header_key. 0, -1 or -2 as
 * above. */
int switchid_title(const char *path, const unsigned char *header_key, uint64_t *id);

/* prod.keys' header_key: two AES-128 keys, 32 bytes. 0, or -1 without one. */
int switchid_header_key(const char *prod_keys, unsigned char key[32]);

/* AES-128-XTS decryption in place, whole sectors from sector on. key is the
 * data key, then the tweak key. Nintendo puts the sector number in the tweak
 * big-endian (nintendo = 1); IEEE 1619 has it little-endian. */
void switchid_xts_decrypt(const unsigned char key[32], uint64_t sector, int nintendo, unsigned char *buf, size_t len,
                          size_t sector_size);

#endif
