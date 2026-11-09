#ifndef N64_H
#define N64_H

#include <stddef.h>

/* N64 games and their saves.
 *
 * Mupen64Plus standalone (Mupen64Plus PS5) keeps an N64 game's save as files
 * of its own, one per kind: <name>.eep, .sra, .fla and .mpk (all four
 * controller paks in one), <name> being "<database name, 32 chars>-<the
 * ROM's MD5, 8 chars>". RomM and the other emulators keep the libretro
 * cores' .srm, everything in one file. RommPS keeps a .srm of the game for
 * it (the shadow) and moves the save between the two. */

/* The ROM's MD5 in .z64 byte order, uppercase hex, as mupen64plus computes it
 * (.v64 and .n64 dumps turned around first). 0, or -1 when it isn't an N64
 * ROM it can read. */
int n64_rom_md5(const char *rom, char out[33]);

/* The name Mupen64Plus gives the game's save files, without the extension:
 * the one already in dir, else made as it makes it, from its database (ini)
 * by MD5, else by the header's CRCs. 0, or -1 when the database doesn't know
 * the game (the name would be a guess). */
int n64_mupen_name(const char *dir, const char *ini, const char *rom, const char *md5, char *out, size_t n);

/* The name gopher64 gives the game's save files, without the extension:
 * the header's name (0x20-0x33, only [a-zA-Z0-9_ -] kept, trimmed; the game
 * id at 0x3B when that leaves nothing), "-", the SHA-256 of the ROM in .z64
 * order in capitals. Its .eep, .sra, .fla and .mpk are Mupen64Plus's. 0, or
 * -1 when it isn't an N64 ROM it can read. */
int n64_gopher_name(const char *rom, char *out, size_t n);

/* The files newer than the shadow: joined into it, the parts of the .srm the
 * game doesn't use kept as they were. 1 when the shadow was written, 0 when
 * there was nothing to do, -1 on an error. */
int n64_fold(const char *dir, const char *name, const char *shadow);

/* The shadow newer than the files (or, force, whatever the dates: RomM's
 * save just put there): what the game uses in it written to them, dated as
 * the shadow. 1 when files were written, 0 when there was nothing to do, -1
 * on an error. */
int n64_unfold(const char *dir, const char *name, const char *shadow, int force);

#endif
