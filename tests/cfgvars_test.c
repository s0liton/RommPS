/* Settings out of emulators' configs, the way each writes them: the catalog's
 * "vars" read memory card, save and game folders with these.
 *   cc -Isrc tests/cfgvars_test.c src/cfgvars.c -o build/host/cfgvars_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfgvars.h"

static int failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);              \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static char v[512];
#define GOT(text, fmt, sec, key, want) (cfgvars_lookup(text, fmt, sec, key, v, sizeof v) && !strcmp(v, want))
#define MISSING(text, fmt, sec, key) (!cfgvars_lookup(text, fmt, sec, key, v, sizeof v))

int main(void) {
    /* PCSX2 and DuckStation: "key = value", the section counts. */
    const char *pcsx2 = "[UI]\nMemoryCards = wrong\n\n[Folders]\nBios = /e/bios\nMemoryCards = /e/saves/pcsx2/saves\r\n"
                        "[GameList]\nRecursivePaths = /e/roms/ps2\nRecursivePaths = /other\n";
    CHECK(GOT(pcsx2, "ini", "Folders", "MemoryCards", "/e/saves/pcsx2/saves"));
    CHECK(GOT(pcsx2, "ini", "GameList", "RecursivePaths", "/e/roms/ps2")); /* a list: the first */
    CHECK(GOT(pcsx2, "ini", NULL, "MemoryCards", "wrong"));                /* no section: the first anywhere */
    CHECK(MISSING(pcsx2, "ini", "Folders", "Memory"));                    /* a key's start isn't the key */
    CHECK(MISSING(pcsx2, "ini", "Nope", "Bios"));

    /* Qt's files (Azahar, Eden): %20 in section names, "key\default" lines
     * that aren't the key, quoted values, and an empty one (the default). */
    const char *qt = "[Data%20Storage]\nnand_directory\\default=false\nnand_directory=/e/storage/eden/nand\n"
                     "save_directory\\default=true\nsave_directory=\n[UI]\nPaths\\gamedirs\\3\\path=\"/e/roms/n3ds\"\n";
    CHECK(GOT(qt, "ini", "Data Storage", "nand_directory", "/e/storage/eden/nand"));
    CHECK(GOT(qt, "ini", "Data%20Storage", "nand_directory", "/e/storage/eden/nand"));
    CHECK(GOT(qt, "ini", "Data Storage", "save_directory", ""));
    CHECK(GOT(qt, "ini", "UI", "Paths\\gamedirs\\3\\path", "/e/roms/n3ds"));

    /* toml: xemu's dotted tables, melonDS's quotes, Xenia's [Profiles]. */
    const char *toml = "[general]\nshow_welcome = false\n[sys.files]\nbootrom_path = '/e/bios/mcpx_1.0.bin'\n"
                       "hdd_path = \"/e/storage/xemu/xbox_hdd.qcow2\"\n[Profiles]\nlogged_profile_slot_0_xuid = \"B13EBABEBABEBABE\"\n";
    CHECK(GOT(toml, "toml", "sys.files", "hdd_path", "/e/storage/xemu/xbox_hdd.qcow2"));
    CHECK(GOT(toml, "toml", "sys.files", "bootrom_path", "/e/bios/mcpx_1.0.bin"));
    CHECK(GOT(toml, "toml", "Profiles", "logged_profile_slot_0_xuid", "B13EBABEBABEBABE"));
    CHECK(MISSING(toml, "toml", "sys", "hdd_path"));

    /* Xenia's toml: every value followed by its description as a comment. */
    const char *xenia = "[Storage]\ncontent_root = \"\"                                                      # Root path for guest content storage (saves, etc.), or empty to use the content folder under the storage root.\n"
                        "cache_root = \"/e/cache # not a comment\"   # Root path for files used to speed up certain parts of the emulator or the game.\n"
                        "mount_cache = false                                                  # Enable cache mount\n"
                        "[Profiles]\nlogged_profile_slot_0_xuid = 'B13EBABEBABEBABE'\t# Profile slot 0\nbare = /e/x#y\n";
    CHECK(GOT(xenia, "toml", "Storage", "content_root", ""));
    CHECK(GOT(xenia, "toml", "Storage", "cache_root", "/e/cache # not a comment"));
    CHECK(GOT(xenia, "toml", "Storage", "mount_cache", "false"));
    CHECK(GOT(xenia, "toml", "Profiles", "logged_profile_slot_0_xuid", "B13EBABEBABEBABE"));
    CHECK(GOT(xenia, "toml", "Profiles", "bare", "/e/x#y")); /* a '#' inside a word isn't a comment */
    CHECK(GOT("[A]\nk=\"/e/q\"  trailing\n", "ini", "A", "k", "/e/q"));
    CHECK(GOT("[A]\nk=/e/a # b\n", "ini", "A", "k", "/e/a # b")); /* ini: no comments after values */
    CHECK(GOT("user-id: 00   # the user\n", "yaml", NULL, "user-id", "00"));

    /* yaml: RPCS3's vfs.yml (keys with slashes, $(EmulatorDir)), Vita3K's config.yml, nesting. */
    const char *vfs = "$(EmulatorDir): \"\"\n/dev_hdd0/: $(EmulatorDir)dev_hdd0/\n/games/: /e/roms/ps3/\n";
    CHECK(GOT(vfs, "yaml", NULL, "/dev_hdd0/", "$(EmulatorDir)dev_hdd0/"));
    CHECK(GOT(vfs, "yaml", NULL, "$(EmulatorDir)", ""));
    CHECK(GOT(vfs, "yaml", NULL, "/games/", "/e/roms/ps3/"));
    const char *nested = "Core:\n  PPU Decoder: Recompiler\nMiscellaneous:\n  pref-path: /wrong\npref-path: '/e/storage/Vita3K/'\n";
    CHECK(GOT(nested, "yaml", NULL, "pref-path", "/wrong"));
    CHECK(GOT(nested, "yaml", "Core", "PPU Decoder", "Recompiler"));
    CHECK(MISSING(nested, "yaml", "Core", "pref-path")); /* past the section's end */

    /* BML: ares's settings.bml, sections as bare names, values after ": ". */
    const char *bml = "Video\n  Driver: OpenGL\nPaths\n  Home\n  Saves: /e/saves/ares/\n  SuperFamicom\n    GameBoy: /e/roms/sgb/\nRecent\n";
    CHECK(GOT(bml, "yaml", "Paths", "Saves", "/e/saves/ares/"));
    CHECK(GOT(bml, "yaml", "SuperFamicom", "GameBoy", "/e/roms/sgb/"));
    CHECK(MISSING(bml, "yaml", "Recent", "Saves"));
    CHECK(MISSING("Paths\n  Saves:\n", "yaml", "Paths", "Home"));
    CHECK(GOT("Path\n  Saves:\n", "yaml", "Path", "Saves", ""));

    /* xml: Cemu's settings.xml, element paths, entities. */
    const char *xml = "<?xml version=\"1.0\"?>\n<content>\n  <mlc_path>/e/roms/wiiu/mlc01</mlc_path>\n"
                      "  <GamePaths>\n    <Entry>/e/roms/wiiu/roms &amp; more</Entry>\n  </GamePaths>\n  <empty/>\n</content>\n";
    CHECK(GOT(xml, "xml", NULL, "mlc_path", "/e/roms/wiiu/mlc01"));
    CHECK(GOT(xml, "xml", NULL, "content/mlc_path", "/e/roms/wiiu/mlc01"));
    CHECK(GOT(xml, "xml", NULL, "content/GamePaths/Entry", "/e/roms/wiiu/roms & more"));
    CHECK(GOT(xml, "xml", NULL, "content/empty", ""));
    CHECK(MISSING(xml, "xml", NULL, "content/nope"));

    /* MAME's mame.ini: "key   value", no "=". */
    const char *mame = "homepath                  $HOME/.mame\nrompath                   /e/roms/arcade;/e/bios\nrompathx  no\n";
    CHECK(GOT(mame, "space", NULL, "rompath", "/e/roms/arcade;/e/bios"));
    CHECK(GOT(mame, "space", NULL, "homepath", "$HOME/.mame"));

    char p[256];
    strcpy(p, "/a/b/../c//./d/");
    cfgvars_normalize(p);
    CHECK(!strcmp(p, "/a/c/d"));
    strcpy(p, "/home/deck/.config/PCSX2/inis/../memcards");
    cfgvars_normalize(p);
    CHECK(!strcmp(p, "/home/deck/.config/PCSX2/memcards"));
    strcpy(p, "/x/{rom_dir}/..");
    cfgvars_normalize(p);
    CHECK(!strcmp(p, "/x"));

    /* config_writes. Eden's [UI] as it writes it: the sizes up top, the rows
     * further down, "\default" lines beside each. */
    const cfgvars_kv sd[] = {{"path", "SDMC"}, {"deep_scan", "false"}, {"expanded", "true"}};
    const cfgvars_kv un[] = {{"path", "UserNAND"}, {"deep_scan", "false"}, {"expanded", "true"}};
    const cfgvars_kv sn[] = {{"path", "SysNAND"}, {"deep_scan", "false"}, {"expanded", "true"}};
    const cfgvars_row seed[] = {{sd, 3}, {un, 3}, {sn, 3}};
    const cfgvars_kv games[] = {{"path", "/e/eden/games"}, {"deep_scan", "false"}, {"expanded", "true"}};
    const cfgvars_row add = {games, 3};
    const char *eden = "[System]\ncurrent_user=0\n\n[UI]\nPaths\\gamedirs\\size=3\nPaths\\external_content_dirs\\size=0\n"
                       "theme=default\nPaths\\gamedirs\\1\\path=SDMC\nPaths\\gamedirs\\1\\deep_scan\\default=true\n"
                       "Paths\\gamedirs\\1\\deep_scan=false\nPaths\\gamedirs\\2\\path=UserNAND\nPaths\\gamedirs\\3\\path=SysNAND\n"
                       "\n[WebService]\nweb_api_url=x\n";
    char *w = cfgvars_array_add(eden, "UI", "Paths\\gamedirs", "path", seed, 3, &add);
    CHECK(w && GOT(w, "ini", "UI", "Paths\\gamedirs\\size", "4") && GOT(w, "ini", "UI", "Paths\\gamedirs\\4\\path", "/e/eden/games"));
    CHECK(w && GOT(w, "ini", "UI", "Paths\\gamedirs\\4\\deep_scan", "false") && GOT(w, "ini", "UI", "Paths\\gamedirs\\1\\path", "SDMC"));
    CHECK(w && strstr(w, "Paths\\gamedirs\\4\\expanded=true\n\n[WebService]")); /* in its section, before the next */
    CHECK(w && !cfgvars_array_add(w, "UI", "Paths\\gamedirs", "path", seed, 3, &add)); /* there already */
    const cfgvars_kv slash[] = {{"path", "/e/eden//games/"}};
    const cfgvars_row add2 = {slash, 1};
    CHECK(w && !cfgvars_array_add(w, "UI", "Paths\\gamedirs", "path", seed, 3, &add2)); /* the same folder */
    free(w);
    /* No list yet: what the emulator puts in an empty one, then ours. */
    w = cfgvars_array_add("[UI]\ntheme=default\n", "UI", "Paths\\gamedirs", "path", seed, 3, &add);
    CHECK(w && GOT(w, "ini", "UI", "Paths\\gamedirs\\size", "4") && GOT(w, "ini", "UI", "Paths\\gamedirs\\3\\path", "SysNAND"));
    CHECK(w && GOT(w, "ini", "UI", "Paths\\gamedirs\\4\\path", "/e/eden/games"));
    free(w);
    /* An empty list with nothing to seed it (the External Content folders), and no [UI] at all. */
    const cfgvars_kv upd[] = {{"path", "/e/eden/updates"}};
    const cfgvars_row add3 = {upd, 1};
    w = cfgvars_array_add(eden, "UI", "Paths\\external_content_dirs", "path", NULL, 0, &add3);
    CHECK(w && GOT(w, "ini", "UI", "Paths\\external_content_dirs\\size", "1") &&
          GOT(w, "ini", "UI", "Paths\\external_content_dirs\\1\\path", "/e/eden/updates"));
    free(w);
    w = cfgvars_array_add("[System]\ncurrent_user=0", "UI", "Paths\\external_content_dirs", "path", NULL, 0, &add3);
    CHECK(w && GOT(w, "ini", "UI", "Paths\\external_content_dirs\\size", "1") && GOT(w, "ini", "System", "current_user", "0"));
    free(w);

    /* DuckStation's list: one more RecursivePaths line, once. */
    w = cfgvars_list_add(pcsx2, "GameList", "RecursivePaths", "/e/roms/psx");
    CHECK(w && strstr(w, "RecursivePaths = /other\nRecursivePaths = /e/roms/psx\n"));
    CHECK(w && !cfgvars_list_add(w, "GameList", "RecursivePaths", "/e/roms/psx/"));
    free(w);
    w = cfgvars_list_add("[Main]\nX = 1\n", "GameList", "RecursivePaths", "/e/roms/psx");
    CHECK(w && !strcmp(w, "[Main]\nX = 1\n\n[GameList]\nRecursivePaths = /e/roms/psx\n"));
    free(w);

    /* One value: set while unset, a folder the user picked stays. */
    const char *rmg = "[Rosalie's Mupen GUI RomBrowser]\nDirectory = \"\"\nRecursive = True\n";
    w = cfgvars_set(rmg, "Rosalie's Mupen GUI RomBrowser", "Directory", "/e/roms/n64", NULL, 1, 1);
    CHECK(w && strstr(w, "Directory = \"/e/roms/n64\"\n"));
    CHECK(w && !cfgvars_set(w, "Rosalie's Mupen GUI RomBrowser", "Directory", "/elsewhere", NULL, 1, 1));
    free(w);
    /* TOML's root table: above the first [table]. */
    w = cfgvars_set("RecentROM = []\n\n[Instance0]\nSaveFilePath = \"\"\n", "", "LastROMFolder", "/e/roms/nds", NULL, 1, 1);
    CHECK(w && !strcmp(w, "RecentROM = []\nLastROMFolder = \"/e/roms/nds\"\n\n[Instance0]\nSaveFilePath = \"\"\n"));
    free(w);

    /* Ryujinx's Config.json: a top-level array of strings, the rest as it was. */
    const char *ryu = "{\n  \"version\": 73,\n  \"game_dirs\": [\n    \"/e/roms/switch\"\n  ],\n  \"autoload_dirs\": [],\n"
                      "  \"hotkeys\": {\n    \"autoload_dirs\": \"nested\"\n  }\n}\n";
    w = cfgvars_json_array_add(ryu, "autoload_dirs", "/e/ryujinx/updates");
    CHECK(w && strstr(w, "\"autoload_dirs\": [\n    \"/e/ryujinx/updates\"\n  ],\n  \"hotkeys\""));
    CHECK(w && !cfgvars_json_array_add(w, "autoload_dirs", "/e/ryujinx/updates/"));
    free(w);
    w = cfgvars_json_array_add(ryu, "game_dirs", "/e/ROMs/switch");
    CHECK(w && strstr(w, "\"game_dirs\": [\n    \"/e/ROMs/switch\",\n    \"/e/roms/switch\"\n  ]"));
    free(w);
    CHECK(!cfgvars_json_array_add(ryu, "game_dirs", "/e/roms/switch"));
    CHECK(!cfgvars_json_array_add(ryu, "version", "/x") && !cfgvars_json_array_add(ryu, "nope", "/x"));

    if (failures) fprintf(stderr, "%d failed\n", failures);
    else printf("cfgvars: all passed\n");
    return failures != 0;
}
