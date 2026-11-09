/* platform.h for the Steam Deck (SteamOS, or any Linux with RetroDECK or EmuDeck).
 *
 * RomM Sync runs as a user service next to the emulators. Emulators come
 * from the catalog (platform/steamdeck/emulators.json), whose paths start at
 * RetroDECK's folders ({rdroms}, {rdsaves}, {rdstates}, {rdbios}, {rdstorage}: wherever
 * its config puts them, the SD card included) and at its Flatpak's own
 * ({rdconfig}, {rddata}); or at EmuDeck's ({edhome}, {edroms}, {edsaves},
 * {edbios}, {edstorage}, {edtools}, {edmedia}), as its settings.sh has them.
 * Either, both or neither can be set up: each one's names are known only
 * while it is, so the other's emulators aren't found. Emulators installed on
 * their own (Flathub, AppImages, EmuDeck's among them) read their folders
 * from their own configs (the catalog's "vars") and keep games in {roms}:
 * EmuDeck's roms folder, else ES-DE's, else ~/ROMs.
 * A "title id" here is a process name (/proc/<pid>/comm, 15 characters at
 * most, or the name of the file it runs), as the catalog's title_ids list
 * them: "pcsx2-qt", "retroarch", "DuckStation.App" (an AppImage's mount).
 *
 * Tests: ROMM_SYNC_DATA sets the data folder, ROMM_SYNC_RDAPP the Flatpak's
 * ~/.var/app folder (where retrodeck.json is), ROMM_SYNC_EDCONFIG EmuDeck's
 * ~/.config/EmuDeck (where settings.sh is), ROMM_SYNC_MEDIA /run/media,
 * ROMM_SYNC_FLATPAK /var/lib/flatpak and ROMM_SYNC_PROC /proc. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "emulators_json.h"
#include "platform.h"
#include "presets.h"
#include "state.h"
#include "util.h"

#define RD_APP_ID "net.retrodeck.retrodeck"
#define FP_RA_APP_ID "org.libretro.RetroArch" /* RetroArch from Flathub (EmuDeck installs this one) */

extern char **environ;

static char g_home[PATH_MAX_LEN];
static char g_data[PATH_MAX_LEN];
static char g_rdapp[PATH_MAX_LEN];    /* ~/.var/app/net.retrodeck.retrodeck */
static char g_rdhome[PATH_MAX_LEN];   /* "" when RetroDECK isn't set up */
static char g_ra_dir[PATH_MAX_LEN];   /* RetroDECK's RetroArch config folder */
static char g_edconfig[PATH_MAX_LEN]; /* ~/.config/EmuDeck */
/* RetroArch's config folders: Flathub's (~/.var/app/org.libretro.RetroArch/
 * config/retroarch) and the one Steam's and the AppImage use (~/.config/retroarch). */
static char g_fp_ra_dir[PATH_MAX_LEN], g_native_ra_dir[PATH_MAX_LEN];
static char g_rd_ra_roms[PATH_MAX_LEN], g_ra_roms[PATH_MAX_LEN]; /* RetroDECK's roms/{esde}, {roms}/{esde} */

static const char *HOMEBREW_DIRS[] = {NULL};
/* RetroDECK's RetroArch, Flathub's and Steam's or the AppImage's, whichever are there. */
static const char *EMULATOR_DIRS[] = {NULL, NULL, NULL, NULL};

static int catalog_var(const char *name, char *out, size_t n);
static int real_path(const char *root, const char *path, char *out, size_t n);
static const char *retroarch_rom_dir(const char *root);
static int retroarch_label(const char *root, char *label, size_t ln, char *name, size_t nn);
static int refresh(void);
static int autostart_enabled(void);

static plat_info_t INFO = {
    .name = "steamdeck",
    .console = "Steam Deck",
    .client = "romm-sync-steamdeck",
    .loader = "systemd",
    .payload = "romm-sync-steamdeck",
    .manifest = "manifest-steamdeck.json",
    .manifest_sig = "manifest-steamdeck.sig",
    .homebrew_dirs = HOMEBREW_DIRS,
    .emulator_dirs = EMULATOR_DIRS,
    .retroarch_root = "",
    .mednafen_root = "",
    .emulator_catalog = EMULATORS_JSON,
    .catalog_var = catalog_var,
    .real_path = real_path,
    .retroarch_rom_dir = retroarch_rom_dir,
    .retroarch_label = retroarch_label,
    .product = "RommDeck",
    .update_source = "GitHub",
    .refresh = refresh,
    .autostart_enabled = autostart_enabled,
    .here = "Deck",
    .peers = "devices",
};

const plat_info_t *plat_info(void) { return &INFO; }

static const char *env_or(const char *name, const char *def) {
    const char *v = getenv(name);
    return v && *v ? v : def;
}

/* "key=value" (RetroDECK's retrodeck.cfg, before 0.10) or "key": "value"
 * (its retrodeck.json): the value of the first line naming key. */
static int config_value(const char *txt, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *l = txt; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        while (*l == ' ' || *l == '\t') l++;
        const char *k = *l == '"' ? l + 1 : l;
        if (strncmp(k, key, kl) != 0) continue;
        const char *v = k + kl;
        if (*v == '"') v++;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '=' && *v != ':') continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        size_t len = strcspn(v, "\"\r\n,");
        while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
        if (!len || len >= n) continue;
        memcpy(out, v, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}

/* RetroDECK's folders. 0.10 names each in retrodeck.json ("paths"), and
 * they needn't be in its home folder (home on the SD card, saves left in
 * ~/retrodeck); before 0.10, retrodeck.cfg named them; failing both, each is
 * <home>/<sub>. The catalog has them as {rdroms}, {rdsaves} and so on. */
static const struct {
    const char *var, *json, *cfg, *sub;
} RD_DIRS[] = {
    {"rdroms", "roms_path", "roms_folder", "roms"},
    {"rdsaves", "saves_path", "saves_folder", "saves"},
    {"rdstates", "states_path", "states_folder", "states"},
    {"rdbios", "bios_path", "bios_folder", "bios"},
    {"rdmedia", "downloaded_media_path", "media_folder", "ES-DE/downloaded_media"},
    {"rdstorage", "storage_path", "storage_folder", "storage"}, /* RPCS3's dev_hdd0 and dev_flash */
};
#define RD_NDIRS (sizeof RD_DIRS / sizeof RD_DIRS[0])
static char g_rd[RD_NDIRS][PATH_MAX_LEN];

enum { RD_ROMS, RD_SAVES };

/* A RetroDECK home folder from before its config named it: its saves are there. */
static int is_rdhome(const char *dir) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, dir, "saves");
    return dir_exists(p);
}

/* Where cards and drives are mounted: SteamOS at /run/media/<label> (before
 * 3.5) or /run/media/<user>/<label>, desktops at the latter. */
static void media_bases(char bases[2][PATH_MAX_LEN]) {
    const char *media = env_or("ROMM_SYNC_MEDIA", "/run/media");
    snprintf(bases[0], PATH_MAX_LEN, "%s", media);
    snprintf(bases[1], PATH_MAX_LEN, "%s/%s", media, env_or("USER", "deck"));
}

/* ~/retrodeck, else a retrodeck folder on a mounted card or drive. */
static void guess_rdhome(void) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, g_home, "retrodeck");
    if (is_rdhome(p)) {
        str_copy(g_rdhome, sizeof g_rdhome, p);
        return;
    }
    char bases[2][PATH_MAX_LEN];
    media_bases(bases);
    for (int b = 0; b < 2 && !g_rdhome[0]; b++) {
        DIR *d = opendir(bases[b]);
        struct dirent *de;
        while (d && !g_rdhome[0] && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            static const char *const names[] = {"retrodeck", "Retrodeck", "RetroDECK"};
            for (size_t i = 0; i < 3 && !g_rdhome[0]; i++) {
                char dir[PATH_MAX_LEN];
                path_join(dir, sizeof dir, bases[b], de->d_name);
                path_join(p, sizeof p, dir, names[i]);
                if (is_rdhome(p)) str_copy(g_rdhome, sizeof g_rdhome, p);
            }
        }
        if (d) closedir(d);
    }
}

/* Where RetroDECK keeps games, saves, states and BIOS files. Set up when its
 * saves folder is there; the games folder appears with the first game. */
static void find_rd_dirs(void) {
    char p[PATH_MAX_LEN];
    g_rdhome[0] = 0;
    for (size_t i = 0; i < RD_NDIRS; i++) g_rd[i][0] = 0;

    path_join(p, sizeof p, g_rdapp, "config/retrodeck/retrodeck.json");
    char *txt = read_file(p, NULL);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const cJSON *paths = cJSON_GetObjectItemCaseSensitive(j, "paths");
    if (paths) {
        str_copy(g_rdhome, sizeof g_rdhome, jget_str(paths, "rd_home_path", ""));
        for (size_t i = 0; i < RD_NDIRS; i++) str_copy(g_rd[i], sizeof g_rd[i], jget_str(paths, RD_DIRS[i].json, ""));
    }
    cJSON_Delete(j);
    if (!paths) {
        path_join(p, sizeof p, g_rdapp, "config/retrodeck/retrodeck.cfg");
        txt = read_file(p, NULL);
        if (txt) {
            config_value(txt, "rdhome", g_rdhome, sizeof g_rdhome);
            for (size_t i = 0; i < RD_NDIRS; i++) config_value(txt, RD_DIRS[i].cfg, g_rd[i], sizeof g_rd[i]);
        }
        free(txt);
    }
    if (!g_rdhome[0]) guess_rdhome();
    for (size_t i = 0; i < RD_NDIRS; i++)
        if (!g_rd[i][0] && g_rdhome[0]) path_join(g_rd[i], sizeof g_rd[i], g_rdhome, RD_DIRS[i].sub);
    if (!g_rd[RD_SAVES][0] || !dir_exists(g_rd[RD_SAVES])) {
        if (g_rd[RD_SAVES][0]) LOGW("RetroDECK: its saves folder %s isn't there: not set up yet?", g_rd[RD_SAVES]);
        g_rdhome[0] = 0;
        for (size_t i = 0; i < RD_NDIRS; i++) g_rd[i][0] = 0;
    }
}

/* EmuDeck's folders, as its settings.sh names them. Each one not named is
 * <emulationPath>/<sub>, as EmuDeck has them by default. */
static const struct {
    const char *var, *key, *sub;
} ED_DIRS[] = {
    {"edhome", "emulationPath", ""},
    {"edroms", "romsPath", "roms"},
    {"edsaves", "savesPath", "saves"},
    {"edbios", "biosPath", "bios"},
    {"edstorage", "storagePath", "storage"},
    {"edtools", "toolsPath", "tools"},
    {"edmedia", "ESDEscrapData", "tools/downloaded_media"}, /* ES-DE's downloaded_media */
};
#define ED_NDIRS (sizeof ED_DIRS / sizeof ED_DIRS[0])
static char g_ed[ED_NDIRS][PATH_MAX_LEN];
static time_t g_ed_mtime; /* settings.sh's, 0 when there's none */
static int g_ed_cloud;    /* EmuDeck's CloudSync is on */

enum { ED_HOME, ED_ROMS, ED_SAVES };

/* A setting from EmuDeck's settings.sh ("romsPath=/run/media/deck/SD/Emulation/roms"),
 * as bash would have it: the last line that sets it counts, quotes come off,
 * and "~", $HOME and ${HOME} are the home folder. Its "move to the SD card"
 * leaves doubled slashes ("Emulation//roms"): those go, and trailing ones.
 * 1 if it's set to something. */
static int ed_setting(const char *txt, const char *key, char *out, size_t n) {
    size_t kl = strlen(key), vl = 0;
    const char *v = NULL;
    for (const char *l = txt; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, "export ", 7)) l += 7;
        if (strncmp(l, key, kl) != 0 || l[kl] != '=') continue;
        v = l + kl + 1;
        vl = strcspn(v, "\r\n");
    }
    if (!v) return 0;
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
    if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) v++, vl -= 2;
    char raw[PATH_MAX_LEN], exp[PATH_MAX_LEN];
    if (!vl || vl >= sizeof raw) return 0;
    memcpy(raw, v, vl);
    raw[vl] = 0;
    size_t o = 0;
    const char *s = raw;
    if (s[0] == '~' && (s[1] == '/' || !s[1])) {
        o = (size_t)snprintf(exp, sizeof exp, "%s", g_home);
        s++;
    }
    while (*s && o < sizeof exp - 1) {
        size_t hl = !strncmp(s, "${HOME}", 7)                                                 ? 7
                    : !strncmp(s, "$HOME", 5) && !isalnum((unsigned char)s[5]) && s[5] != '_' ? 5
                                                                                              : 0;
        if (!hl) {
            exp[o++] = *s++;
            continue;
        }
        o += (size_t)snprintf(exp + o, sizeof exp - o, "%s", g_home);
        s += hl;
    }
    if (o >= sizeof exp - 1 && *s) return 0;
    exp[o < sizeof exp ? o : sizeof exp - 1] = 0;
    size_t w = 0;
    for (size_t i = 0; exp[i] && w < n - 1; i++)
        if (exp[i] != '/' || !w || out[w - 1] != '/') out[w++] = exp[i];
    while (w > 1 && out[w - 1] == '/') w--;
    out[w] = 0;
    return w > 0;
}

/* EmuDeck's settings: ~/.config/EmuDeck/settings.sh, or ~/emudeck/settings.sh
 * from before it moved there. */
static int ed_settings_path(char *out, size_t n) {
    path_join(out, n, g_edconfig, "settings.sh");
    if (file_exists(out)) return 1;
    snprintf(out, n, "%s/emudeck/settings.sh", g_home);
    return file_exists(out);
}

/* An Emulation folder EmuDeck made: games, saves and its launchers. (A card
 * labelled "Emulation" can hold RetroDECK instead: it has none of those.) */
static int is_edhome(const char *dir) {
    static const char *const subs[] = {"roms", "saves", "tools/launchers"};
    char p[PATH_MAX_LEN];
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        path_join(p, sizeof p, dir, subs[i]);
        if (!dir_exists(p)) return 0;
    }
    return 1;
}

/* No settings.sh: ~/Emulation, else an Emulation folder on a card or drive. */
static void guess_edhome(char *out, size_t n) {
    char p[PATH_MAX_LEN], bases[2][PATH_MAX_LEN];
    out[0] = 0;
    path_join(p, sizeof p, g_home, "Emulation");
    if (is_edhome(p)) {
        str_copy(out, n, p);
        return;
    }
    media_bases(bases);
    for (int b = 0; b < 2 && !out[0]; b++) {
        DIR *d = opendir(bases[b]);
        struct dirent *de;
        while (d && !out[0] && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char dir[PATH_MAX_LEN];
            path_join(dir, sizeof dir, bases[b], de->d_name);
            path_join(p, sizeof p, dir, "Emulation");
            if (is_edhome(p)) str_copy(out, n, p);
        }
        if (d) closedir(d);
    }
}

/* Where EmuDeck keeps games, saves, BIOS files and the rest. Set up when its
 * saves folder is there (not while the card it's on is out). */
static void find_ed_dirs(void) {
    char p[PATH_MAX_LEN], v[16];
    for (size_t i = 0; i < ED_NDIRS; i++) g_ed[i][0] = 0;
    g_ed_mtime = 0;
    g_ed_cloud = 0;
    struct stat st;
    char *txt = ed_settings_path(p, sizeof p) ? read_file(p, NULL) : NULL;
    if (txt) {
        if (stat(p, &st) == 0) g_ed_mtime = st.st_mtime;
        for (size_t i = 0; i < ED_NDIRS; i++) ed_setting(txt, ED_DIRS[i].key, g_ed[i], sizeof g_ed[i]);
        g_ed_cloud = ed_setting(txt, "cloud_sync_status", v, sizeof v) && !strcmp(v, "true");
        if (!g_ed[ED_HOME][0] && !g_ed[ED_SAVES][0])
            path_join(g_ed[ED_HOME], sizeof g_ed[ED_HOME], g_home, "Emulation");
    } else {
        guess_edhome(g_ed[ED_HOME], sizeof g_ed[ED_HOME]);
    }
    free(txt);
    /* Saves named but not the folder above them: it's their parent. */
    if (!g_ed[ED_HOME][0] && g_ed[ED_SAVES][0]) {
        str_copy(g_ed[ED_HOME], sizeof g_ed[ED_HOME], g_ed[ED_SAVES]);
        char *slash = strrchr(g_ed[ED_HOME], '/');
        if (slash && slash > g_ed[ED_HOME]) *slash = 0;
    }
    for (size_t i = 1; i < ED_NDIRS; i++)
        if (!g_ed[i][0] && g_ed[ED_HOME][0]) path_join(g_ed[i], sizeof g_ed[i], g_ed[ED_HOME], ED_DIRS[i].sub);
    if (!g_ed[ED_SAVES][0] || !dir_exists(g_ed[ED_SAVES])) {
        if (g_ed_mtime)
            LOGW("EmuDeck: its saves folder %s isn't there: not set up yet, or its card is out?", g_ed[ED_SAVES]);
        for (size_t i = 0; i < ED_NDIRS; i++) g_ed[i][0] = 0;
    }
}

/* EmuDeck's CloudSync uploads and downloads the same saves this does: said
 * once, and again if it's turned off and back on. */
static void ed_cloud_sync_check(void) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, g_data, "emudeck-cloudsync-said");
    if (!g_ed[ED_SAVES][0] || !g_ed_cloud) {
        if (g_ed[ED_SAVES][0]) unlink(p);
        return;
    }
    if (file_exists(p)) return;
    plat_notify("%s: EmuDeck's CloudSync is on\nIt syncs these saves too: keep one of the two on", INFO.product);
    write_file_atomic(p, "", 0);
}

/* Where emulators installed on their own keep games ({roms}/<system>, as
 * ES-DE names systems): EmuDeck's roms folder when it's set up, else the one
 * ES-DE is set to, else ES-DE's default, ~/ROMs. */
static void roms_dir(char *out, size_t n) {
    if (g_ed[ED_SAVES][0]) {
        str_copy(out, n, g_ed[ED_ROMS]);
        return;
    }
    static const char *const settings[] = {"ES-DE/settings/es_settings.xml", ".emulationstation/es_settings.xml"};
    for (size_t i = 0; i < sizeof settings / sizeof settings[0]; i++) {
        char p[PATH_MAX_LEN], v[PATH_MAX_LEN];
        path_join(p, sizeof p, g_home, settings[i]);
        char *txt = read_file(p, NULL);
        const char *at = txt ? strstr(txt, "name=\"ROMDirectory\"") : NULL;
        const char *val = at ? strstr(at, "value=\"") : NULL;
        size_t l = val ? strcspn(val + 7, "\"\n") : 0;
        if (l && l < sizeof v) {
            snprintf(v, sizeof v, "%.*s", (int)l, val + 7);
            if (v[0] == '~') snprintf(out, n, "%s%s", g_home, v + 1);
            else str_copy(out, n, v);
            size_t ol = strlen(out);
            while (ol > 1 && out[ol - 1] == '/') out[--ol] = 0;
            free(txt);
            return;
        }
        free(txt);
    }
    path_join(out, n, g_home, "ROMs");
}

static int catalog_var(const char *name, char *out, size_t n) {
    if (!strcmp(name, "home")) {
        str_copy(out, n, g_home);
        return 1;
    }
    if (!strcmp(name, "roms")) {
        roms_dir(out, n);
        return 1;
    }
    if (!strcmp(name, "fpsys")) { /* system-wide Flatpaks (Flathub's emulators, RetroDECK) */
        str_copy(out, n, env_or("ROMM_SYNC_FLATPAK", "/var/lib/flatpak"));
        return 1;
    }
    /* RetroDECK's names only while it's set up: its emulators aren't found otherwise. */
    for (size_t i = 0; i < RD_NDIRS; i++)
        if (!strcmp(name, RD_DIRS[i].var)) {
            if (!g_rd[RD_SAVES][0]) return 0;
            str_copy(out, n, g_rd[i]);
            return 1;
        }
    if (!strcmp(name, "rdconfig") || !strcmp(name, "rddata")) {
        if (!g_rd[RD_SAVES][0]) return 0;
        path_join(out, n, g_rdapp, name + 2);
        return 1;
    }
    /* EmuDeck's the same way. */
    for (size_t i = 0; i < ED_NDIRS; i++)
        if (!strcmp(name, ED_DIRS[i].var)) {
            if (!g_ed[ED_SAVES][0]) return 0;
            str_copy(out, n, g_ed[i]);
            return 1;
        }
    return 0;
}

/* The Flatpak a folder is in (~/.var/app/<id>/...), RetroDECK's wherever its
 * folder is: that folder and the app's id. 0 if it's in none. */
static int flatpak_of(const char *root, char *app, size_t an, char *id, size_t idn) {
    size_t l = strlen(g_rdapp);
    if (!root) return 0;
    if (!strncmp(root, g_rdapp, l) && (root[l] == '/' || !root[l])) {
        str_copy(app, an, g_rdapp);
        str_copy(id, idn, RD_APP_ID);
        return 1;
    }
    const char *at = strstr(root, "/.var/app/");
    if (!at) return 0;
    const char *s = at + 10;
    size_t il = strcspn(s, "/");
    if (!il || il >= idn) return 0;
    snprintf(app, an, "%.*s", (int)(s + il - root), root);
    snprintf(id, idn, "%.*s", (int)il, s);
    return 1;
}

/* A Flatpak's /app on this machine: a system install or a user one (EmuDeck's). */
static int flatpak_files(const char *id, char *out, size_t n) {
    snprintf(out, n, "%s/app/%s/current/active/files", env_or("ROMM_SYNC_FLATPAK", "/var/lib/flatpak"), id);
    if (dir_exists(out)) return 1;
    snprintf(out, n, "%s/.local/share/flatpak/app/%s/current/active/files", g_home, id);
    return dir_exists(out);
}

/* Inside a Flatpak, its config and data folders are /var/config and
 * /var/data, and the app itself is /app: those of the one root is in
 * (RetroDECK's RetroArch or EmuDeck's). "~" is the home folder. */
static int real_path(const char *root, const char *path, char *out, size_t n) {
    static const struct {
        const char *in, *sub;
    } MAP[] = {{"/var/config", "config"}, {"/var/data", "data"}, {"/var/cache", "cache"}};
    char app[PATH_MAX_LEN], id[256], files[PATH_MAX_LEN];
    const int sandboxed = flatpak_of(root, app, sizeof app, id, sizeof id);
    for (size_t i = 0; sandboxed && i < sizeof MAP / sizeof MAP[0]; i++) {
        size_t l = strlen(MAP[i].in);
        if (strncmp(path, MAP[i].in, l) != 0 || (path[l] != '/' && path[l])) continue;
        snprintf(out, n, "%s/%s%s", app, MAP[i].sub, path + l);
        return 1;
    }
    if (sandboxed && !strncmp(path, "/app/", 5) && flatpak_files(id, files, sizeof files)) {
        snprintf(out, n, "%s%s", files, path + 4);
        return 1;
    }
    if (path[0] == '~' && (path[1] == '/' || !path[1])) {
        snprintf(out, n, "%s%s", g_home, path + 1);
        return 1;
    }
    return 0;
}

/* Each RetroArch that's there: RetroDECK's (when it's set up), Flathub's
 * (EmuDeck's, or one installed on its own) and Steam's or the AppImage's. Its
 * config folder, and its games in RetroDECK's roms/<system> or {roms}/<system>.
 * RetroDECK's comes first: the built-in profile's folder. */
static void setup_retroarch(void) {
    int k = 0;
    char roms[PATH_MAX_LEN];
    path_join(g_ra_dir, sizeof g_ra_dir, g_rdapp, "config/retroarch");
    snprintf(g_fp_ra_dir, sizeof g_fp_ra_dir, "%s/.var/app/" FP_RA_APP_ID "/config/retroarch", g_home);
    snprintf(g_native_ra_dir, sizeof g_native_ra_dir, "%s/.config/retroarch", g_home);
    roms_dir(roms, sizeof roms);
    snprintf(g_rd_ra_roms, sizeof g_rd_ra_roms, "%s/{esde}", g_rd[RD_ROMS]);
    snprintf(g_ra_roms, sizeof g_ra_roms, "%s/{esde}", roms);
    char cfg[PATH_MAX_LEN];
    path_join(cfg, sizeof cfg, g_native_ra_dir, "retroarch.cfg");
    EMULATOR_DIRS[0] = EMULATOR_DIRS[1] = EMULATOR_DIRS[2] = NULL;
    if (g_rd[RD_SAVES][0] && dir_exists(g_ra_dir)) EMULATOR_DIRS[k++] = g_ra_dir;
    if (dir_exists(g_fp_ra_dir)) EMULATOR_DIRS[k++] = g_fp_ra_dir;
    if (file_exists(cfg)) EMULATOR_DIRS[k++] = g_native_ra_dir;
    INFO.retroarch_root = k ? EMULATOR_DIRS[0] : "";
}

static const char *retroarch_rom_dir(const char *root) {
    if (!root) return NULL;
    if (g_rd[RD_SAVES][0] && !strcmp(root, g_ra_dir)) return g_rd_ra_roms;
    if (!strcmp(root, g_fp_ra_dir) || !strcmp(root, g_native_ra_dir)) return g_ra_roms;
    return NULL;
}

/* They all keep their config in a "retroarch" folder. RetroDECK's keeps the
 * "retroarch-retroarch" it always had; Flathub's is "retroarch-flatpak", and
 * just "RetroArch" when RetroDECK's isn't there to tell apart. */
static int retroarch_label(const char *root, char *label, size_t ln, char *name, size_t nn) {
    if (!root) return 0;
    if (!strcmp(root, g_fp_ra_dir)) {
        str_copy(label, ln, "flatpak");
        str_copy(name, nn, g_rd[RD_SAVES][0] && dir_exists(g_ra_dir) ? "RetroArch (Flatpak)" : "RetroArch");
        return 1;
    }
    if (!strcmp(root, g_native_ra_dir)) {
        str_copy(label, ln, "native");
        str_copy(name, nn, "RetroArch (Steam or AppImage)");
        return 1;
    }
    return 0;
}

/* RetroDECK's and EmuDeck's folders again (detection runs this): an SD card
 * mounted after the session started, either set up since, or EmuDeck's
 * settings changed. 1 if they changed. */
static int refresh(void) {
    char rd_before[RD_NDIRS][PATH_MAX_LEN], ed_before[ED_NDIRS][PATH_MAX_LEN], roms_before[PATH_MAX_LEN],
        roms[PATH_MAX_LEN];
    const time_t mtime_before = g_ed_mtime;
    memcpy(rd_before, g_rd, sizeof rd_before);
    memcpy(ed_before, g_ed, sizeof ed_before);
    str_copy(roms_before, sizeof roms_before, g_ra_roms);
    find_rd_dirs();
    find_ed_dirs();
    setup_retroarch();
    ed_cloud_sync_check();
    const int rd = memcmp(rd_before, g_rd, sizeof rd_before) != 0, ed = memcmp(ed_before, g_ed, sizeof ed_before) != 0;
    /* ES-DE pointed at another games folder: {roms} with it. */
    const int moved = strcmp(roms_before, g_ra_roms) != 0;
    if (moved && !ed) {
        roms_dir(roms, sizeof roms);
        LOGI("Games for emulators of their own now go to %s", roms);
    }
    if (rd)
        LOGI("RetroDECK: games in %s, saves in %s (found again)", g_rd[RD_ROMS][0] ? g_rd[RD_ROMS] : "-",
             g_rd[RD_SAVES][0] ? g_rd[RD_SAVES] : "-");
    if (ed)
        LOGI("EmuDeck: games in %s, saves in %s (found again)", g_ed[ED_ROMS][0] ? g_ed[ED_ROMS] : "-",
             g_ed[ED_SAVES][0] ? g_ed[ED_SAVES] : "-");
    if (!ed && g_ed_mtime != mtime_before) LOGI("EmuDeck: its settings changed, its folders didn't");
    return rd || ed || moved || g_ed_mtime != mtime_before;
}

/* The systemd user unit install.sh enables. */
static int autostart_enabled(void) {
    char p[PATH_MAX_LEN];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(p, sizeof p, "%s/systemd/user/default.target.wants/romm-sync.service", xdg);
    else snprintf(p, sizeof p, "%s/.config/systemd/user/default.target.wants/romm-sync.service", g_home);
    return file_exists(p);
}

/* One copy at a time: a lock on a file in the data folder, held for life. */
static int another_copy_runs(void) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, g_data, "romm-sync.lock");
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return 0; /* fd stays open: the lock goes with the process */
    close(fd);
    return errno == EWOULDBLOCK;
}

int plat_init(void) {
    signal(SIGPIPE, SIG_IGN);
    str_copy(g_home, sizeof g_home, env_or("HOME", "/home/deck"));
    char def[PATH_MAX_LEN];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) path_join(def, sizeof def, xdg, "romm-sync");
    else snprintf(def, sizeof def, "%s/.local/share/romm-sync", g_home);
    str_copy(g_data, sizeof g_data, env_or("ROMM_SYNC_DATA", def));
    mkdir_p(g_data);

    char rdapp[PATH_MAX_LEN];
    snprintf(rdapp, sizeof rdapp, "%s/.var/app/" RD_APP_ID, g_home);
    str_copy(g_rdapp, sizeof g_rdapp, env_or("ROMM_SYNC_RDAPP", rdapp));
    char edconfig[PATH_MAX_LEN];
    snprintf(edconfig, sizeof edconfig, "%s/.config/EmuDeck", g_home);
    str_copy(g_edconfig, sizeof g_edconfig, env_or("ROMM_SYNC_EDCONFIG", edconfig));
    find_rd_dirs();
    find_ed_dirs();
    setup_retroarch();
    if (another_copy_runs()) return 1;
    if (g_rd[RD_SAVES][0]) LOGI("RetroDECK: games in %s, saves in %s", g_rd[RD_ROMS], g_rd[RD_SAVES]);
    else LOGI("RetroDECK: not found");
    if (g_ed[ED_SAVES][0]) LOGI("EmuDeck: games in %s, saves in %s", g_ed[ED_ROMS], g_ed[ED_SAVES]);
    else LOGI("EmuDeck: not found");
    ed_cloud_sync_check();
    return 0;
}

/* A desktop notification when there's a session to show it (Desktop Mode;
 * Game Mode shows none), and the log either way. */
void plat_notify(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    LOGI("notify: %s", msg);
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || getenv("ROMM_SYNC_NO_NOTIFY")) return;
    char *title = msg, *body = strchr(msg, '\n');
    if (body) *body++ = 0;
    char *argv[] = {"notify-send", "--app-name=RommDeck", title, body ? body : "", NULL};
    pid_t pid;
    if (posix_spawnp(&pid, "notify-send", NULL, NULL, argv, environ) == 0) waitpid(pid, NULL, 0);
}

/* Each process's names, for the title ids the catalog knows: its comm (15
 * characters: "DuckStation.App" for DuckStation.AppImage's mount, a Windows
 * program's own name under Wine), then, when that says nothing, the name of
 * the file it runs and its first 15 characters (an emulator that renames its
 * main thread). Only this user's processes: the emulators run as the user. */
typedef int (*proc_fn)(const char *name, void *ctx);

static int each_process(proc_fn fn, void *ctx) {
    const char *proc = env_or("ROMM_SYNC_PROC", "/proc");
    DIR *d = opendir(proc);
    if (!d) return -1;
    struct dirent *de;
    uid_t me = getuid();
    int r = 0;
    while (!r && (de = readdir(d))) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        char p[PATH_MAX_LEN], comm[32] = "";
        snprintf(p, sizeof p, "%s/%s", proc, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0 || st.st_uid != me) continue;
        snprintf(p, sizeof p, "%s/%s/comm", proc, de->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\r\n")] = 0;
        fclose(f);
        if (comm[0] && (r = fn(comm, ctx))) break;
        char exe[PATH_MAX_LEN], first[16];
        snprintf(p, sizeof p, "%s/%s/exe", proc, de->d_name);
        ssize_t l = readlink(p, exe, sizeof exe - 1);
        if (l <= 0) continue;
        exe[l] = 0;
        if (l > 10 && !strcmp(exe + l - 10, " (deleted)")) exe[l - 10] = 0; /* updated while it ran */
        const char *base = path_basename(exe);
        if (base[0] && strcmp(base, comm) != 0) r = fn(base, ctx);
        snprintf(first, sizeof first, "%.15s", base);
        if (!r && strlen(base) > 15 && strcmp(first, comm) != 0) r = fn(first, ctx);
    }
    closedir(d);
    return r;
}

/* RetroArch has no catalog entry: setup finds it from its config. */
static int known_title(const char *comm) {
    if (!strcmp(comm, "retroarch")) return 1;
    const cJSON *e, *t;
    cJSON_ArrayForEach(e, presets_catalog())
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(
                                  e, "title_ids")) if (cJSON_IsString(t) && !strcmp(t->valuestring, comm)) return 1;
    return 0;
}

typedef struct {
    char *out;
    size_t n;
} found_ctx;

static int take_known(const char *comm, void *ctx) {
    found_ctx *c = ctx;
    if (!known_title(comm)) return 0;
    str_copy(c->out, c->n, comm);
    return 1;
}

/* No foreground app to ask for: an emulator that runs is the game. */
int plat_running_title(char *title_id, size_t n) {
    title_id[0] = 0;
    found_ctx c = {title_id, n};
    int r = each_process(take_known, &c);
    return r < 0 ? -1 : r;
}

static int same_name(const char *comm, void *ctx) { return !strcmp(comm, ctx); }

int plat_app_alive(const char *title_id) {
    if (!title_id || !title_id[0]) return -1;
    return each_process(same_name, (void *)title_id);
}

const char *plat_data_dir(void) { return g_data; }

/* libcurl uses the system's CA certificates. */
const char *plat_ca_bundle(void) {
    const char *c = getenv("ROMM_SYNC_CA");
    return c && *c ? c : NULL;
}

void plat_remove_old_tile(void) {}

/* RommDeck, which install.sh puts beside this program's copy. */
int plat_app_installed(void) {
    char p[PATH_MAX_LEN];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) snprintf(p, sizeof p, "%s/rommdeck/rommdeck", xdg);
    else snprintf(p, sizeof p, "%s/.local/share/rommdeck/rommdeck", g_home);
    return file_exists(p);
}

void plat_app_version(char *out, size_t n) { str_copy(out, n, ""); }

int plat_launch_elf(const void *elf, size_t len) {
    (void)elf;
    (void)len;
    errno = ENOSYS;
    return -1;
}

void plat_mounts_set(const plat_mount *mounts, int n) {
    (void)mounts;
    (void)n;
}

int plat_is_mount(const char *path) {
    (void)path;
    return 0;
}

const char *plat_name(void) { return INFO.name; }

void plat_console_name(char *buf, size_t n) {
    const char *env = getenv("ROMM_SYNC_CONSOLE_NAME");
    if (env) {
        snprintf(buf, n, "%s", env);
        return;
    }
    if (gethostname(buf, n) != 0) buf[0] = 0;
    else buf[n - 1] = 0;
}

void plat_describe(char *buf, size_t n) {
    char os[128] = "Linux";
    char *txt = read_file("/etc/os-release", NULL);
    const char *pretty = txt ? strstr(txt, "PRETTY_NAME=") : NULL;
    if (pretty) {
        pretty += 12;
        if (*pretty == '"') pretty++;
        snprintf(os, sizeof os, "%.*s", (int)strcspn(pretty, "\"\n"), pretty);
    }
    free(txt);
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(buf, n, "%s, kernel %s %s, RetroDECK %s, EmuDeck %s", os, u.release, u.machine,
                 g_rd[RD_SAVES][0] ? g_rdhome : "not found", g_ed[ED_SAVES][0] ? g_ed[ED_HOME] : "not found");
    else snprintf(buf, n, "%s", os);
}

void plat_local_ip(char *buf, size_t n) {
    buf[0] = 0;
    struct ifaddrs *ifs, *i;
    if (getifaddrs(&ifs) != 0) return;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
        if (ntohl(a->sin_addr.s_addr) >> 24 == 127) continue;
        inet_ntop(AF_INET, &a->sin_addr, buf, (socklen_t)n);
        break;
    }
    freeifaddrs(ifs);
}
