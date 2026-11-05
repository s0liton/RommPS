/* platform.h for the Steam Deck (SteamOS, or any Linux with RetroDECK).
 *
 * RomM Sync runs as a user service next to the emulators. Emulators come
 * from the catalog (platform/steamdeck/emulators.json), whose paths start at
 * RetroDECK's folders ({rdroms}, {rdsaves}, {rdstates}, {rdbios}: wherever
 * its config puts them, the SD card included) and at its Flatpak's own
 * ({rdconfig}, {rddata}).
 * A "title id" here is a process name (/proc/<pid>/comm, 15 characters at
 * most), as the catalog's title_ids list them: "pcsx2-qt", "retroarch".
 *
 * Tests: ROMM_SYNC_DATA sets the data folder, ROMM_SYNC_RDAPP the Flatpak's
 * ~/.var/app folder (where retrodeck.json is) and ROMM_SYNC_PROC /proc. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
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

extern char **environ;

static char g_home[PATH_MAX_LEN];
static char g_data[PATH_MAX_LEN];
static char g_rdapp[PATH_MAX_LEN];   /* ~/.var/app/net.retrodeck.retrodeck */
static char g_rdhome[PATH_MAX_LEN];  /* "" when RetroDECK isn't set up */
static char g_rdfiles[PATH_MAX_LEN]; /* the Flatpak's /app, "" if not found */
static char g_ra_dir[PATH_MAX_LEN];  /* RetroDECK's RetroArch config folder */

static const char *HOMEBREW_DIRS[] = {NULL};
static const char *EMULATOR_DIRS[] = {NULL, NULL}; /* g_ra_dir, when it's there */

static int catalog_var(const char *name, char *out, size_t n);
static int real_path(const char *path, char *out, size_t n);

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

/* ~/retrodeck, else a retrodeck folder on a mounted card or drive.
 * SteamOS mounts cards at /run/media/<label>, desktops at /run/media/<user>/<label>. */
static void guess_rdhome(void) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, g_home, "retrodeck");
    if (is_rdhome(p)) {
        str_copy(g_rdhome, sizeof g_rdhome, p);
        return;
    }
    char bases[2][PATH_MAX_LEN];
    snprintf(bases[0], sizeof bases[0], "/run/media");
    snprintf(bases[1], sizeof bases[1], "/run/media/%s", env_or("USER", "deck"));
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

/* The Flatpak's /app on this machine: a system install or a user one. */
static void find_rdfiles(void) {
    char p[PATH_MAX_LEN];
    g_rdfiles[0] = 0;
    snprintf(p, sizeof p, "/var/lib/flatpak/app/" RD_APP_ID "/current/active/files");
    if (dir_exists(p)) {
        str_copy(g_rdfiles, sizeof g_rdfiles, p);
        return;
    }
    snprintf(p, sizeof p, "%s/.local/share/flatpak/app/" RD_APP_ID "/current/active/files", g_home);
    if (dir_exists(p)) str_copy(g_rdfiles, sizeof g_rdfiles, p);
}

static int catalog_var(const char *name, char *out, size_t n) {
    if (!strcmp(name, "home")) {
        str_copy(out, n, g_home);
        return 1;
    }
    if (!g_rd[RD_SAVES][0]) return 0; /* no RetroDECK: its emulators aren't found */
    for (size_t i = 0; i < RD_NDIRS; i++)
        if (!strcmp(name, RD_DIRS[i].var)) {
            str_copy(out, n, g_rd[i]);
            return 1;
        }
    if (!strcmp(name, "rdconfig") || !strcmp(name, "rddata")) {
        path_join(out, n, g_rdapp, name + 2);
        return 1;
    }
    return 0;
}

/* Inside RetroDECK's Flatpak, its config and data folders are /var/config
 * and /var/data, and the app itself is /app. "~" is the home folder. */
static int real_path(const char *path, char *out, size_t n) {
    static const struct {
        const char *in, *sub;
    } MAP[] = {{"/var/config", "config"}, {"/var/data", "data"}, {"/var/cache", "cache"}};
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        size_t l = strlen(MAP[i].in);
        if (strncmp(path, MAP[i].in, l) != 0 || (path[l] != '/' && path[l])) continue;
        snprintf(out, n, "%s/%s%s", g_rdapp, MAP[i].sub, path + l);
        return 1;
    }
    if (!strncmp(path, "/app/", 5) && g_rdfiles[0]) {
        snprintf(out, n, "%s%s", g_rdfiles, path + 4);
        return 1;
    }
    if (path[0] == '~' && (path[1] == '/' || !path[1])) {
        snprintf(out, n, "%s%s", g_home, path + 1);
        return 1;
    }
    return 0;
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
    find_rd_dirs();
    find_rdfiles();
    path_join(g_ra_dir, sizeof g_ra_dir, g_rdapp, "config/retroarch");
    if (g_rd[RD_SAVES][0] && dir_exists(g_ra_dir)) {
        static char roms[PATH_MAX_LEN];
        snprintf(roms, sizeof roms, "%s/{esde}", g_rd[RD_ROMS]);
        EMULATOR_DIRS[0] = g_ra_dir;
        INFO.retroarch_root = g_ra_dir;
        INFO.retroarch_rom_dir = roms;
    }
    if (another_copy_runs()) return 1;
    if (g_rd[RD_SAVES][0]) LOGI("RetroDECK: games in %s, saves in %s", g_rd[RD_ROMS], g_rd[RD_SAVES]);
    else LOGI("RetroDECK: not found");
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
    char *argv[] = {"notify-send", "--app-name=RomM Sync", title, body ? body : "", NULL};
    pid_t pid;
    if (posix_spawnp(&pid, "notify-send", NULL, NULL, argv, environ) == 0) waitpid(pid, NULL, 0);
}

/* Each process's name, for the title ids the catalog knows. Only this
 * user's processes: the emulators run as the user. */
typedef int (*proc_fn)(const char *comm, void *ctx);

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
        if (comm[0]) r = fn(comm, ctx);
    }
    closedir(d);
    return r;
}

/* RetroArch has no catalog entry: setup finds it from its config. */
static int known_title(const char *comm) {
    if (!strcmp(comm, "retroarch")) return 1;
    const cJSON *e, *t;
    cJSON_ArrayForEach(e, presets_catalog())
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(e, "title_ids"))
            if (cJSON_IsString(t) && !strcmp(t->valuestring, comm)) return 1;
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

/* The Deck's own app isn't written yet. */
int plat_app_installed(void) { return 0; }

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
    if (uname(&u) == 0) snprintf(buf, n, "%s, kernel %s %s, RetroDECK %s", os, u.release, u.machine, g_rd[RD_SAVES][0] ? g_rdhome : "not found");
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
