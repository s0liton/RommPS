/* DuckStation's memory cards, one per game, named as it names them
 * (stenzek/duckstation core/system.cpp, GetMemoryCardPathForSlot and
 * UpdateRunningGame; RetroDECK's DuckStation is an older build of it, its
 * differences noted where they matter). A card is <its folder>/<NAME>_<slot>.mcd,
 * NAME by the slot's Card<N>Type in settings.ini (the game's own
 * gamesettings/<serial>.ini over it):
 *   PerGameTitle      the game's title in DuckStation's database, its disc
 *                     set's for a game on several discs (UsePlaylistTitle,
 *                     unless a card for the one disc is there already); the
 *                     file's name for a game the database doesn't know
 *   PerGame           the game's serial, the database's for it
 *   PerGameFileTitle  the file's name
 *   Shared            one card every game uses; None, NonPersistent: none
 * Its characters as DuckStation keeps them on Linux: "/", "*" and control
 * characters become "_".
 *
 * The database is DuckStation's own, read from the copy installed with it
 * (gamedb.yaml, and discsets.yaml in newer builds). It's under a licence of
 * its own, so none of it is shipped, sent anywhere or kept: only the name a
 * game's card gets, worked out here. */
#ifndef ROMM_DUCKCARD_H
#define ROMM_DUCKCARD_H

#include <stddef.h>

/* What a slot's settings make of a game's card. */
typedef enum {
    DUCK_OFF = 0,  /* no card (None, NonPersistent) */
    DUCK_SHARED,   /* the card every game shares */
    DUCK_PER_GAME, /* a card of its own: the name says which */
} duck_kind;

/* A slot's settings: settings.ini's [MemoryCards], then the game's own ini
 * (gamesettings/<serial>.ini, by its first disc's serial for a set). */
typedef struct {
    char type[24];      /* Card<N>Type: "PerGameTitle" (slot 1's default), "None" (slot 2's)... */
    int playlist_title; /* UsePlaylistTitle: one card for every disc of a set */
} duck_slot;

void duck_read_slot(const char *data_root, const char *ini_serial, int slot, duck_slot *out);

/* The database, as read. */
typedef struct duck_db duck_db;

typedef struct {
    char serial[24];     /* the entry's own, which a disc's other codes lead to */
    char title[256];     /* the title its cards are named by (saveName, else name) */
    char set_title[256]; /* its disc set's, "" outside one */
    char set_first[24];  /* the set's first disc */
} duck_entry;

/* From the files' text (discsets NULL: an older build's, sets inside entries). */
duck_db *duck_db_parse(const char *gamedb, const char *discsets);
void duck_db_free(duck_db *db);
/* The entry a disc's code leads to. 1 if there's one. */
int duck_db_find(const duck_db *db, const char *code, duck_entry *out);
/* An older build (no discsets.yaml): RetroDECK's. */
int duck_db_legacy(const duck_db *db);
size_t duck_db_size(const duck_db *db);

/* The database of the DuckStation keeping its settings in data_root: in the
 * first of dirs (comma separated folders holding gamedb.yaml: where a
 * Flatpak has it) that has one, else the newest copy in `copies` taken from
 * one running with that data_root (duck_probe_running). Read once while the
 * files stay as they are. NULL when there's none. */
const duck_db *duck_db_for(const char *dirs, const char *data_root, const char *copies);

/* An AppImage keeps its files in itself, readable only while it runs (its
 * mount, /tmp/.mount_*): for each DuckStation running, under proc (/proc),
 * its database copied to copies/<AppImage's size>-<mtime>, with the data
 * root it uses, and older copies for that data root removed. At most every
 * few seconds; the copy is made once per version. */
void duck_probe_running(const char *proc, const char *copies);

/* One game, as DuckStation is given it. */
typedef struct {
    const char *boot;      /* the file it starts: a disc image, an .m3u, a .pbp */
    const char *serial;    /* the disc's code ("SCUS-94163"), "" unread */
    const char *data_root; /* its folder (settings.ini, gamesettings/, custom_properties.ini) */
    const char *card_dir;  /* [MemoryCards] Directory */
} duck_game;

/* NAME_<slot> (no ".mcd") of the game's card in a slot. *sure is 0 when it's
 * a guess: no database to read, a .pbp, no serial read for a card named by
 * one; then out is the likeliest name (duck_fallback's). */
duck_kind duck_card_stem(const duck_db *db, const duck_game *g, int slot, char *out, size_t n, int *sure);

/* The card in dir for slot that holds saves of one of serials (a PS1 save
 * carries its game's code), when exactly one does. 1 if found, out its stem. */
int duck_card_by_owner(const char *dir, int slot, const char *const *serials, int nserials, char *out, size_t n);

/* A name without the database: the first of <stem>_N, <stem without
 * " (Disc N)">_N and <serial>_N that dir has, else the disc set's (stem
 * without the disc) when the stem names one, else <stem>_N. */
void duck_fallback(const char *dir, const char *stem, const char *serial, int slot, char *out, size_t n);

/* name with "/", "*" and control characters as "_", as DuckStation names files. */
void duck_sanitize(char *name);

/* The first disc an .m3u lists, as a path (relative ones beside it). 0 if any. */
int duck_m3u_first(const char *m3u, char *out, size_t n);

#endif
