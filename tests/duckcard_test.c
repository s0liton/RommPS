/* src/duckcard.c: DuckStation's memory card names (make unit). The database
 * here is made up (serials, titles): never a copy of DuckStation's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "duckcard.h"
#include "util.h"

static int failures;
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
        } \
    } while (0)

/* The newer layout: entries, and sets in discsets.yaml. */
static const char *GAMEDB = "ABCD-00001:\n"
                            "  name: \"Alpha Quest (Disc 1)\"\n"
                            "  saveName: \"Alpha Quest (Testland) (Disc 1)\"\n"
                            "  codes:\n"
                            "    - ABCD-00001\n"
                            "    - ABCD-90001\n"
                            "  metadata:\n"
                            "    languages:\n"
                            "      - English\n"
                            "ABCD-00002:\n"
                            "  name: \"Alpha Quest (Disc 2)\"\n"
                            "  saveName: \"Alpha Quest (Testland) (Disc 2)\"\n"
                            "  traits:\n"
                            "    - SomeTrait # a comment\n"
                            "ABCD-00010:\n"
                            "  name: \"Beta / Gamma*\"\n"
                            "WXYZ-00003:\n"
                            "  name: 'It''s Delta'\n"
                            "WXYZ-00004:\n"
                            "  name: \"Quoted \\\"Epsilon\\\"\"\n"
                            "  codes:\n"
                            "    - WXYZ-00004\n"
                            "    - ABCD-00001\n";
static const char *DISCSETS = "- name: \"Alpha Quest\"\n"
                              "  saveName: \"Alpha Quest (Testland)\"\n"
                              "  serials:\n"
                              "    - ABCD-00001\n"
                              "    - ABCD-00002\n";
/* The older one (RetroDECK's build): the set inside each of its entries. */
static const char *LEGACY = "ABCD-00001:\n"
                            "  name: \"Alpha Quest (Testland) (Disc 1)\"\n"
                            "  discSet:\n"
                            "    name: \"Alpha Quest (Testland)\"\n"
                            "    serials:\n"
                            "      - ABCD-00001\n"
                            "      - ABCD-00002\n"
                            "ABCD-00002:\n"
                            "  name: \"Alpha Quest (Testland) (Disc 2)\"\n"
                            "  discSet:\n"
                            "    name: \"Alpha Quest (Testland)\"\n"
                            "    serials:\n"
                            "      - ABCD-00001\n"
                            "      - ABCD-00002\n"
                            "ABCD-00010:\n"
                            "  name: \"Beta Game\"\n";

static char g_dir[256];

static void put(const char *rel, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, rel);
    mkdir_parent(path);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void rm(const char *rel) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, rel);
    unlink(path);
}

static void settings(const char *memcards) {
    char txt[512];
    snprintf(txt, sizeof txt, "[Main]\nSettingsVersion = 3\n\n[MemoryCards]\n%s\n", memcards);
    put("root/settings.ini", txt);
}

/* A raw PS1 card with one save of `owner`'s ("BAABCD-00001..."). */
static void ps1_card(const char *rel, const char *save_name) {
    static unsigned char img[128 * 1024];
    memset(img, 0, sizeof img);
    img[0] = 'M', img[1] = 'C';
    for (int f = 1; f < 16; f++) {
        unsigned char *fr = img + f * 128;
        fr[0] = f == 1 && save_name ? 0x51 : 0xA0;
        if (f == 1 && save_name) {
            fr[5] = 0x20; /* 8192 */
            memcpy(fr + 0x0A, save_name, strlen(save_name));
        }
        fr[8] = fr[9] = 0xFF;
    }
    for (int f = 0; f < 16; f++) {
        unsigned char x = 0, *fr = img + f * 128;
        for (int i = 0; i < 127; i++) x ^= fr[i];
        fr[127] = x;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, rel);
    mkdir_parent(path);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(img, 1, sizeof img, fp);
        fclose(fp);
    }
}

static duck_kind name(const duck_db *db, const char *boot, const char *serial, char *out, int *sure) {
    char root[300], cards[300], b[300];
    snprintf(root, sizeof root, "%s/root", g_dir);
    snprintf(cards, sizeof cards, "%s/cards", g_dir);
    snprintf(b, sizeof b, "%s/roms/%s", g_dir, boot);
    const duck_game g = {b, serial, root, cards};
    return duck_card_stem(db, &g, 1, out, 256, sure);
}

#define NAMED(db, boot, serial, want) \
    do { \
        char got[256]; \
        int sure = -1; \
        duck_kind k = name(db, boot, serial, got, &sure); \
        CHECK(k == DUCK_PER_GAME && !strcmp(got, want) && sure == 1, "%s: \"%s\" (kind %d, sure %d), want \"%s\"", \
              boot, got, (int)k, sure, want); \
    } while (0)

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/duckcard-test-XXXXXX");
    if (!mkdtemp(g_dir)) return 1;
    char cards[300];
    snprintf(cards, sizeof cards, "%s/cards", g_dir);
    mkdir_p(cards);

    /* The database. */
    duck_db *db = duck_db_parse(GAMEDB, DISCSETS);
    duck_entry e;
    CHECK(db && duck_db_size(db) == 5 && !duck_db_legacy(db), "parse: %zu entries", duck_db_size(db));
    CHECK(duck_db_find(db, "ABCD-90001", &e) && !strcmp(e.serial, "ABCD-00001") &&
              !strcmp(e.title, "Alpha Quest (Testland) (Disc 1)") && !strcmp(e.set_title, "Alpha Quest (Testland)") &&
              !strcmp(e.set_first, "ABCD-00001"),
          "another code of a disc: %s / %s / %s / %s", e.serial, e.title, e.set_title, e.set_first);
    CHECK(duck_db_find(db, "ABCD-00001", &e) && !strcmp(e.serial, "ABCD-00001"),
          "a code twice: the first entry keeps it");
    CHECK(duck_db_find(db, "ABCD-00002", &e) && !strcmp(e.set_first, "ABCD-00001"), "disc 2's set");
    CHECK(duck_db_find(db, "WXYZ-00003", &e) && !strcmp(e.title, "It's Delta") && !e.set_title[0], "single quotes: %s",
          e.title);
    CHECK(duck_db_find(db, "WXYZ-00004", &e) && !strcmp(e.title, "Quoted \"Epsilon\""), "escapes: %s", e.title);
    CHECK(!duck_db_find(db, "SomeTrait", &e) && !duck_db_find(db, "English", &e) && !duck_db_find(db, "ZZZZ-00000", &e),
          "only codes lead to entries");
    duck_db *old = duck_db_parse(LEGACY, NULL);
    CHECK(old && duck_db_legacy(old) && duck_db_find(old, "ABCD-00002", &e) &&
              !strcmp(e.set_title, "Alpha Quest (Testland)") && !strcmp(e.set_first, "ABCD-00001"),
          "older layout's set");

    /* PerGameTitle, the default. */
    settings("Card1Type = PerGameTitle");
    NAMED(db, "Alpha Quest (Testland) (Disc 1).chd", "ABCD-00001", "Alpha Quest (Testland)_1");
    NAMED(db, "aq2.chd", "ABCD-00002", "Alpha Quest (Testland)_1");
    NAMED(db, "anything.cue", "ABCD-90001", "Alpha Quest (Testland)_1");
    NAMED(db, "beta.chd", "ABCD-00010", "Beta _ Gamma__1");
    NAMED(db, "Unknown Thing (Europe).chd", "QQQQ-12345", "Unknown Thing (Europe)_1");
    NAMED(db, "My Alpha.m3u", "ABCD-00001", "Alpha Quest (Testland)_1");
    /* A disc's own card, there already, stays the one it uses. */
    put("cards/Alpha Quest (Testland) (Disc 2)_1.mcd", "x");
    NAMED(db, "aq2.chd", "ABCD-00002", "Alpha Quest (Testland) (Disc 2)_1");
    NAMED(db, "aq1.chd", "ABCD-00001", "Alpha Quest (Testland)_1");
    rm("cards/Alpha Quest (Testland) (Disc 2)_1.mcd");
    settings("Card1Type = PerGameTitle\nUsePlaylistTitle = false");
    NAMED(db, "aq1.chd", "ABCD-00001", "Alpha Quest (Testland) (Disc 1)_1");

    /* RetroDECK's build: a playlist's card is the playlist's name. */
    settings("Card1Type = PerGameTitle\nUsePlaylistTitle = true");
    NAMED(old, "My Alpha.m3u", "ABCD-00001", "My Alpha_1");
    NAMED(old, "aq1.chd", "ABCD-00001", "Alpha Quest (Testland)_1");
    NAMED(old, "b.chd", "ABCD-00010", "Beta Game_1");
    NAMED(old, "Unknown.m3u", "QQQQ-12345", "Unknown_1");

    /* The other types. */
    settings("Card1Type = PerGame");
    NAMED(db, "x.chd", "ABCD-90001", "ABCD-00001_1");
    NAMED(db, "x.chd", "QQQQ-12345", "QQQQ-12345_1");
    settings("Card1Type = PerGameFileTitle");
    NAMED(db, "Some * File.chd", "ABCD-00001", "Some _ File_1");
    char got[256];
    int sure;
    settings("Card1Type = Shared");
    CHECK(name(db, "x.chd", "ABCD-00001", got, &sure) == DUCK_SHARED, "Shared");
    settings("Card1Type = None");
    CHECK(name(db, "x.chd", "ABCD-00001", got, &sure) == DUCK_OFF, "None");
    settings("Card1Type = NonPersistent");
    CHECK(name(db, "x.chd", "ABCD-00001", got, &sure) == DUCK_OFF, "NonPersistent");
    settings("Card1Type = Bogus");
    NAMED(db, "aq1.chd", "ABCD-00001", "Alpha Quest (Testland)_1");

    /* A game's own settings (a set's: its first disc's), and a title the user gave it. */
    settings("Card1Type = PerGameTitle");
    put("root/gamesettings/ABCD-00001.ini", "[MemoryCards]\nCard1Type = PerGameFileTitle\n");
    NAMED(db, "disc two.chd", "ABCD-00002", "disc two_1");
    put("root/gamesettings/ABCD-00001.ini", "[MemoryCards]\nCard1Type = None\n");
    CHECK(name(db, "aq1.chd", "ABCD-00001", got, &sure) == DUCK_OFF, "a game's own None");
    rm("root/gamesettings/ABCD-00001.ini");
    char props[1024];
    snprintf(props, sizeof props, "[%s/roms/renamed.chd]\nTitle = My Own Name\n", g_dir);
    put("root/custom_properties.ini", props);
    NAMED(db, "renamed.chd", "ABCD-00010", "My Own Name_1");
    rm("root/custom_properties.ini");

    /* No database: a guess, the files there first. */
    CHECK(name(NULL, "Alpha Quest (Testland) (Disc 1).chd", "ABCD-00001", got, &sure) == DUCK_PER_GAME && !sure &&
              !strcmp(got, "Alpha Quest (Testland)_1"),
          "no database, a set's disc: %s", got);
    put("cards/ABCD-00001_1.mcd", "x");
    CHECK(name(NULL, "Alpha Quest (Testland) (Disc 1).chd", "ABCD-00001", got, &sure) == DUCK_PER_GAME && !sure &&
              !strcmp(got, "ABCD-00001_1"),
          "no database, the serial's card there: %s", got);
    CHECK(name(db, "game.pbp", "", got, &sure) == DUCK_PER_GAME && !sure && !strcmp(got, "game_1"), "a .pbp: %s", got);

    /* Cards by whose saves they hold. */
    ps1_card("cards/Some Name_1.mcd", "BAABCD-00001ALPHA01");
    ps1_card("cards/Other_1.mcd", "BAWXYZ-00003DELTA");
    ps1_card("cards/shared_card_1.mcd", "BAABCD-00001ALPHA01");
    ps1_card("cards/Empty_1.mcd", NULL);
    const char *want[] = {"ABCD-00002", "ABCD-00001"};
    CHECK(duck_card_by_owner(cards, 1, want, 2, got, sizeof got) && !strcmp(got, "Some Name_1"), "by owner: %s", got);
    const char *none[] = {"QQQQ-12345"};
    CHECK(!duck_card_by_owner(cards, 1, none, 1, got, sizeof got), "no card has it");
    ps1_card("cards/Copy_1.mcd", "BAABCD-00001ALPHA01");
    CHECK(!duck_card_by_owner(cards, 1, want, 2, got, sizeof got), "two cards have it: neither");

    /* .m3u, and the rest. */
    put("roms/set.m3u", "# a comment\n\n  Alpha (Disc 1).chd  \nAlpha (Disc 2).chd\n");
    char first[512], expect[512];
    snprintf(expect, sizeof expect, "%s/roms/Alpha (Disc 1).chd", g_dir);
    snprintf(first, sizeof first, "%s/roms/set.m3u", g_dir);
    CHECK(duck_m3u_first(first, first, sizeof first) == 0 && !strcmp(first, expect), "m3u: %s", first);
    char s[64] = "a/b*c\td";
    duck_sanitize(s);
    CHECK(!strcmp(s, "a_b_c_d"), "sanitize: %s", s);

    duck_db_free(db);
    duck_db_free(old);
    remove_tree(g_dir);
    if (failures) {
        fprintf(stderr, "duckcard: %d failed\n", failures);
        return 1;
    }
    printf("duckcard: ok\n");
    return 0;
}
