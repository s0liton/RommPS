/* Settings read out of an emulator's own config file, for the catalog's
 * "vars" (src/presets.c): where it keeps memory cards, saves or games. Simple
 * key lookups, not parsers: enough for the files emulators write. */
#ifndef ROMM_CFGVARS_H
#define ROMM_CFGVARS_H

#include <stddef.h>

/* The value of key in a config file's text, in one of these formats:
 *   "ini"   [section] then key=value or key = value. Qt's files too: quoted
 *           values ("...") and %20 in section names ([Data%20Storage]).
 *   "toml"  [section] (dotted, as written: "sys.files") then key = 'value'.
 *   "yaml"  key: value, under "section:" when one is given (RPCS3's vfs.yml
 *           keys, "/dev_hdd0/", work as they are). BML too (ares, bsnes):
 *           a section there is the bare name on its line.
 *   "xml"   <key>value</key>; key may be a path of elements ("content/mlc_path").
 *   "space" key value, whitespace between (MAME's mame.ini).
 * section may be NULL or "": the first key with that name anywhere. A quoted
 * value is what's between its quotes, whatever follows; in toml and yaml an
 * unquoted one ends at a '#' after a space (a comment). 1 if found (the value
 * may be empty), 0 if not. */
int cfgvars_lookup(const char *text, const char *format, const char *section, const char *key, char *out, size_t n);

/* path with "." and ".." taken out, slashes not doubled and none at the end
 * ("/a/b/../c//" is "/a/c"). In place. */
void cfgvars_normalize(char *path);

/* Changes to a config's text, for the catalog's "config_writes" (cfgwrite.c):
 * a games folder added to the emulator's list of them. Each returns the new
 * text (malloc'd), or NULL when it already says so (or can't be changed).
 * Lines are added at the end of their [section], which is made when it isn't
 * there; "" is the top of the file, before any section (TOML's root table).
 * Paths compare with "." "..", doubled and trailing slashes taken out. */
typedef struct {
    const char *key, *value;
} cfgvars_kv;
typedef struct {
    const cfgvars_kv *kv;
    int n;
} cfgvars_row;

/* A Qt array (QSettings, and the yuzu forks' files): "<array>\size=N" and
 * "<array>\<i>\<key>=value", i from 1. add goes in as row N+1, unless a row's
 * `match` key already has add's value. An array missing or empty gets seed's
 * rows first (what the emulator itself puts in an empty list). */
char *cfgvars_array_add(const char *text, const char *section, const char *array, const char *match,
                        const cfgvars_row *seed, int nseed, const cfgvars_row *add);

/* A list kept as the same key on several lines ("RecursivePaths = /a" then
 * "RecursivePaths = /b"): one more line, unless one has value already. */
char *cfgvars_list_add(const char *text, const char *section, const char *key, const char *value);

/* A JSON file's top-level array of strings ("autoload_dirs": [...] in
 * Ryujinx's Config.json): value goes in first, unless it's there already.
 * The rest of the text stays as it was. NULL too when key isn't an array. */
char *cfgvars_json_array_add(const char *text, const char *key, const char *value);

/* One key's value: replaced, or added as key<sep>value (sep " = " when NULL);
 * quoted in "..." when quote. if_unset: only while it's missing or empty (a
 * folder the user picked stays). */
char *cfgvars_set(const char *text, const char *section, const char *key, const char *value, const char *sep, int quote,
                  int if_unset);

#endif
