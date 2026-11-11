#include "library.h"
#include "sync.h"
#include "covers.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "events.h"
#include "firmware.h"
#include "http.h"
#include "platform.h"
#include "profiles.h"
#include "romm.h"
#include "state.h"
#include "switchid.h"
#include "util.h"

#define MAX_WORKERS 8
#define MAX_JOBS    50

typedef enum { JOB_ROM, JOB_FIRMWARE } job_type;
typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_ERROR, JOB_CANCELLED, JOB_SYNCING } job_status;

typedef struct job {
    int id;
    job_type type;
    int target_id; /* rom id or platform id */
    int disc;      /* one disc of a game kept as a folder of discs; 0: all of it */
    char name[256];
    char cover[160]; /* RomM's path_cover_small, for the queue row */
    char platform[64];
    job_status status;
    int cancel;
    char error[256];
    int64_t done, total;
    int64_t base; /* bytes from files already finished (multi-file games) */
    double speed; /* bytes/s, smoothed */
    char note[80]; /* Running job updates this after a download. */
    sync_game_result saves; /* what the sync after the download brought */
    int when_back;            /* failed with RomM not answering: goes again once it does */
    double sample_t;
    int64_t sample_bytes;
    struct job *next;
} job;

static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;
static job *g_jobs; /* newest first */
static int g_job_seq, g_running;

static int finished(const job *j) { return j->status == JOB_DONE || j->status == JOB_ERROR || j->status == JOB_CANCELLED; }

static job *oldest_queued(void) {
    job *pick = NULL;
    for (job *j = g_jobs; j; j = j->next)
        if (j->status == JOB_QUEUED) pick = j;
    return pick;
}

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* A game came or went through us: the installed fingerprint (library_installed_sig) is worked out again. */
static volatile int g_sig_stale = 1;
static void installed_changed(void) { g_sig_stale = 1; }

static int progress_cb(void *ud, int64_t done, int64_t total) {
    job *j = ud;
    double now = mono_now();
    pthread_mutex_lock(&g_q_lock);
    j->done = j->base + done;
    if (total > 0 && j->base + total > j->total) j->total = j->base + total;
    if (j->sample_t == 0) {
        j->sample_t = now;
        j->sample_bytes = j->done;
    } else if (now - j->sample_t >= 1.0) {
        double inst = (double)(j->done - j->sample_bytes) / (now - j->sample_t);
        j->speed = j->speed > 0 ? j->speed * 0.6 + inst * 0.4 : inst;
        j->sample_t = now;
        j->sample_bytes = j->done;
    }
    int stop = j->cancel;
    pthread_mutex_unlock(&g_q_lock);
    return stop;
}

static int cancelled(job *j) {
    pthread_mutex_lock(&g_q_lock);
    int c = j->cancel;
    pthread_mutex_unlock(&g_q_lock);
    return c;
}

/* Whether RomM is away: known not to answer, or not answering now. A
 * transfer cut off half way doesn't say so by itself, hence the ask. */
static int romm_away(void) {
    char why[160];
    if (romm_no_answer(why, sizeof why)) return 1;
    long st = 0;
    cJSON_Delete(romm_call("GET", "/api/heartbeat", NULL, &st));
    return st == 0 && romm_no_answer(why, sizeof why);
}

static void job_end(job *j, job_status st, const char *fmt, const char *arg) {
    const int away = st == JOB_ERROR && !cancelled(j) && romm_away();
    pthread_mutex_lock(&g_q_lock);
    j->status = j->cancel ? JOB_CANCELLED : st;
    if (fmt && j->status == JOB_ERROR) snprintf(j->error, sizeof j->error, fmt, arg ? arg : "");
    if (j->status == JOB_ERROR && away) {
        j->when_back = 1;
        str_copy(j->error, sizeof j->error, "RomM isn't answering. This starts again once it does");
    }
    if (j->status == JOB_DONE) j->done = j->total;
    pthread_mutex_unlock(&g_q_lock);
    if (j->status == JOB_ERROR) LOGE("download of %s failed: %s", j->name, j->error);
    if (j->status == JOB_CANCELLED) LOGI("download of %s cancelled", j->name);
}

static int load_maps(profile_map **maps) {
    config_lock();
    int n = profiles_expand(g_cfg.profiles, maps);
    config_unlock();
    return n;
}

/* Consoles by their makers' names ("Sony PlayStation", "Microsoft Xbox"), by
 * any of the slugs RomM gives them; RomM's own name for the rest. The app's
 * Emulator Settings (setup_page.cpp) and the web page (ui/index.html) keep
 * the same names by system. */
static const struct {
    const char *slugs, *name;
} CONSOLE_NAMES[] = {
    {"psx,ps1,playstation", "Sony PlayStation"},
    {"ps2", "Sony PlayStation 2"},
    {"ps3", "Sony PlayStation 3"},
    {"psp", "Sony PlayStation Portable"},
    {"psvita", "Sony PlayStation Vita"},
    {"xbox", "Microsoft Xbox"},
    {"xbox360", "Microsoft Xbox 360"},
    {"nes,famicom", "Nintendo Entertainment System"},
    {"fds", "Nintendo Famicom Disk System"},
    {"snes,sfam", "Super Nintendo Entertainment System"},
    {"satellaview", "Nintendo Satellaview"},
    {"sufami-turbo", "Bandai SuFami Turbo"},
    {"gb", "Nintendo Game Boy"},
    {"gbc", "Nintendo Game Boy Color"},
    {"gba", "Nintendo Game Boy Advance"},
    {"n64", "Nintendo 64"},
    {"nds", "Nintendo DS"},
    {"3ds", "Nintendo 3DS"},
    {"virtualboy", "Nintendo Virtual Boy"},
    {"pokemon-mini", "Nintendo Pokémon Mini"},
    {"ngc,gamecube", "Nintendo GameCube"},
    {"wii", "Nintendo Wii"},
    {"wiiu", "Nintendo Wii U"},
    {"switch", "Nintendo Switch"},
    {"genesis,genesis-slash-megadrive,megadrive", "Sega Mega Drive / Genesis"},
    {"sms,mastersystem", "Sega Master System"},
    {"gamegear,gg", "Sega Game Gear"},
    {"segacd,sega-cd,megacd", "Sega CD / Mega-CD"},
    {"sega32,sega-32x", "Sega 32X"},
    {"sg1000", "Sega SG-1000"},
    {"sega-pico", "Sega Pico"},
    {"saturn", "Sega Saturn"},
    {"dc,dreamcast", "Sega Dreamcast"},
    {"tg16,turbografx16--1,pce,pcengine", "NEC PC Engine / TurboGrafx-16"},
    {"turbografx-cd,pcecd", "NEC PC Engine CD / TurboGrafx-CD"},
    {"supergrafx", "NEC SuperGrafx"},
    {"pc-fx", "NEC PC-FX"},
    {"neo-geo-pocket,ngp", "SNK Neo Geo Pocket"},
    {"neo-geo-pocket-color,ngpc", "SNK Neo Geo Pocket Color"},
    {"neo-geo-cd", "SNK Neo Geo CD"},
    {"neogeoaes", "SNK Neo Geo AES"},
    {"neogeomvs", "SNK Neo Geo MVS"},
    {"wonderswan,ws", "Bandai WonderSwan"},
    {"wonderswan-color,wsc", "Bandai WonderSwan Color"},
    {"atari2600", "Atari 2600"},
    {"atari5200", "Atari 5200"},
    {"atari7800", "Atari 7800"},
    {"lynx", "Atari Lynx"},
    {"jaguar", "Atari Jaguar"},
    {"c64", "Commodore 64"},
    {"amiga", "Commodore Amiga"},
    {"amiga-cd32", "Commodore Amiga CD32"},
    {"commodore-cdtv", "Commodore CDTV"},
    {"3do", "Panasonic 3DO"},
    {"philips-cd-i", "Philips CD-i"},
    {"colecovision", "Coleco ColecoVision"},
    {"channel-f,fairchild-channel-f", "Fairchild Channel F"},
    {"zxs,zx-spectrum", "Sinclair ZX Spectrum"},
    {"dos", "MS-DOS"},
};

static int slug_in(const char *list, const char *slug) {
    const size_t n = strlen(slug);
    for (const char *t = list; *t;) {
        const char *end = strchr(t, ',');
        const size_t l = end ? (size_t)(end - t) : strlen(t);
        if (l == n && !strncmp(t, slug, n)) return 1;
        if (!end) break;
        t = end + 1;
    }
    return 0;
}

/* A platform by its maker's name when the table knows it, else RomM's
 * (name_key), else its slug. p is a platform, or a ROM ("platform_slug"...). */
static const char *console_name(const cJSON *p, const char *slug_key, const char *fs_key, const char *name_key) {
    const char *slugs[2] = {jget_str(p, slug_key, ""), jget_str(p, fs_key, "")};
    for (size_t i = 0; i < sizeof CONSOLE_NAMES / sizeof *CONSOLE_NAMES; i++)
        for (int k = 0; k < 2; k++)
            if (slugs[k][0] && slug_in(CONSOLE_NAMES[i].slugs, slugs[k])) return CONSOLE_NAMES[i].name;
    const char *name = jget_str(p, name_key, "");
    return name[0] ? name : slugs[0];
}

/* No emulator found because the list of them couldn't be built (memory, on
 * the PS4), not because none plays the platform: say which. */
#define MAPS_SHORT_MSG "the console was short of memory just then. Try again in a moment"
static int maps_failed(const profile_map *maps, int n) {
    config_lock();
    const int have = cJSON_GetArraySize(g_cfg.profiles) > 0;
    config_unlock();
    return !maps && !n && have;
}

/* Files a RomM scan picks up beside the games (a frontend's metadata.txt or
 * gamelist.xml, artwork, readmes, checksums) aren't games: the library hides
 * them and they aren't downloaded. Not .md: that's a Mega Drive ROM. */
#define NOT_GAME_EXTS                                                                                                  \
    ".txt,.nfo,.diz,.pdf,.htm,.html,.xml,.json,.ini,.log,.url,.lnk,.jpg,.jpeg,.png,.gif,.bmp,.webp,.mp4,.mkv,"        \
    ".mp3,.flac,.ogg,.sfv,.md5,.sha1,.crc,.db"

/* Inside a game's folder fewer are left out, since a game can need its own
 * .xml, .ini or .db: only readmes, artwork and checksums, and only loose ones
 * (leave_rules). */
#define NOT_NEEDED_EXTS ".txt,.nfo,.diz,.pdf,.url,.lnk,.jpg,.jpeg,.png,.gif,.bmp,.webp,.sfv,.md5,.sha1"

int library_not_game(const char *fs_name) { return ext_in_list(fs_name, NOT_GAME_EXTS); }

/* What RomM files as a game's extras: its manuals, soundtracks, screenshots,
 * walkthroughs and cheats. */
static int extra_category(const cJSON *f) {
    const char *cat = jget_str(f, "category", "");
    static const char *skip[] = {"manual", "walkthrough", "soundtrack", "screenshot", "cheat"};
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++)
        if (!strcmp(cat, skip[i])) return 1;
    return 0;
}

/* A file in a game's folder that playing it doesn't need: RomM's extras, and
 * the files above. */
static int extra_file(const cJSON *f) {
    return extra_category(f) || ext_in_list(jget_str(f, "file_name", ""), NOT_NEEDED_EXTS);
}

/* The folder within a game's that a file of it goes in, as RomM's file_path
 * has it below "<fs_path>/<fs_name>" (a GOD game's 415607E1/00007000), ""
 * for the game's own. Never anywhere out of the game's folder. */
static const char *game_subdir(const cJSON *rom, const cJSON *f) {
    char top[PATH_MAX_LEN];
    snprintf(top, sizeof top, "%s/%s", jget_str(rom, "fs_path", ""), jget_str(rom, "fs_name", ""));
    const char *fp = jget_str(f, "file_path", "");
    const size_t tl = strlen(top);
    if (!strncmp(fp, top, tl) && fp[tl] == '/' && fp[tl + 1] && !strstr(fp + tl, "/../") && !str_ends_with_ci(fp, "/.."))
        return fp + tl + 1;
    return "";
}

/* PC platforms (RomM's slugs, and the folder names people give them): a game
 * folder there is the game's own file tree, and nobody can say which .txt or
 * .png it does without (gbe_fork's steam_settings/steam_appid.txt, for one).
 * Their games come down whole. */
#define KEEP_ALL_PLATFORMS "win,windows,win3x,win9x,windows-apps,dos,pc,linux,mac,os2,beos,steam,steam-vr,scummvm"

/* What a download of a game leaves on RomM (left_out). Elsewhere RomM's
 * extras stay there, and readmes, artwork and checksums only when they're
 * loose beside the game's files: in a folder of its own a .png is the game's
 * data (PS3_GAME/ICON0.PNG, a Wii U game's content/), not its box art. */
typedef struct {
    int all;           /* every file: keep_all_files, or a PC platform's game */
    int anywhere;      /* readmes, artwork and checksums wherever they are: keep_all_files false */
    const char *loose; /* else only those in this folder of the game's (game_subdir) */
} leave_rules;

static leave_rules leave_rules_for(const profile_map *m, const cJSON *rom) {
    leave_rules r = {m->keep_all_files == 1, m->keep_all_files == 0, ""};
    for (int k = 0; m->keep_all_files < 0 && k < 2; k++) {
        char slug[64];
        str_copy(slug, sizeof slug, jget_str(rom, k ? "platform_fs_slug" : "platform_slug", ""));
        for (char *c = slug; *c; c++) *c = (char)tolower((unsigned char)*c);
        r.all |= slug_in(KEEP_ALL_PLATFORMS, slug);
    }
    if (r.all || r.anywhere) return r;
    /* The game's own folder, unless every file of it sits in one folder
     * inside it, as a game unpacked into a folder of its own does
     * ("Game/Game/readme.txt"). A folder with folders in it is the game's
     * tree, readmes and all. */
    const cJSON *f;
    int first = 1;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files")) {
        const char *in = game_subdir(rom, f);
        if (first) {
            r.loose = in;
            first = 0;
        } else if (strcmp(in, r.loose) != 0) {
            r.loose = "";
            break;
        }
    }
    return r;
}

static int left_out(const leave_rules *r, const cJSON *rom, const cJSON *f) {
    if (r->all) return 0;
    if (extra_category(f)) return 1;
    return ext_in_list(jget_str(f, "file_name", ""), NOT_NEEDED_EXTS) &&
           (r->anywhere || !strcmp(game_subdir(rom, f), r->loose));
}

/* Archives nothing on the console plays from: a .rar anywhere, and any
 * archive for a Switch game (RetroArch opens a .zip or .7z itself). The
 * game's archive's extension, or NULL. */
#define ARCHIVED_MSG "This download is archived (%s). Decompress it on your RomM server first."
static const char *rom_archive(const cJSON *rom, int nx) {
    const char *list = nx ? ".rar,.zip,.7z" : ".rar";
    const char *fs_name = jget_str(rom, "fs_name", "");
    if (ext_in_list(fs_name, list)) return path_ext(fs_name);
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files"))
        if (!extra_file(f) && ext_in_list(jget_str(f, "file_name", ""), list)) return path_ext(jget_str(f, "file_name", ""));
    return NULL;
}

/* Switch games come with their updates and DLC, which RomM keeps as games of
 * their own or as files in a game's folder. Each file names its title id
 * ("Game [0100000011D90800][v393216].nsp"): a game's ends in 000, its
 * update's is the same + 0x800 and its DLC's + 0x1001 on, so clearing the
 * low 13 bits gives the game's. The game goes in rom_dir, the rest in
 * update_dir and dlc_dir, and the library shows only the game. */
typedef enum { NX_NONE, NX_GAME, NX_UPDATE, NX_DLC } nx_kind;
#define NX_MAX_FILES 64

static int is_switch(const char *slug) { return !strcmp(slug, "switch"); }

static nx_kind nx_title(const char *name, unsigned long long *game, unsigned long *version) {
    nx_kind kind = NX_NONE;
    *game = 0;
    for (const char *p = strchr(name, '['); p && kind == NX_NONE; p = strchr(p + 1, '[')) {
        if (strncmp(p + 1, "01", 2) != 0 || strspn(p + 1, "0123456789abcdefABCDEF") != 16) continue;
        char hex[17];
        memcpy(hex, p + 1, 16);
        hex[16] = 0;
        const unsigned long long id = strtoull(hex, NULL, 16);
        const unsigned low = (unsigned)(id & 0x1FFF);
        kind = low == 0 ? NX_GAME : low == 0x800 ? NX_UPDATE : low > 0x1000 ? NX_DLC : NX_NONE;
        if (kind != NX_NONE) *game = id & ~0x1FFFULL;
    }
    if (version) {
        *version = 0;
        for (const char *v = strstr(name, "[v"); v; v = strstr(v + 1, "[v"))
            if (v[2] >= '0' && v[2] <= '9') {
                *version = strtoul(v + 2, NULL, 10);
                break;
            }
    }
    return kind;
}

/* The title id a name carries ("0100ABF008968800"), for showing; "" if none. */
static const char *nx_title_hex(const char *name, char out[17]) {
    out[0] = 0;
    for (const char *p = strchr(name, '['); p; p = strchr(p + 1, '['))
        if (!strncmp(p + 1, "01", 2) && strspn(p + 1, "0123456789abcdefABCDEF") == 16) {
            for (int i = 0; i < 16; i++) out[i] = (char)toupper((unsigned char)p[1 + i]);
            out[16] = 0;
            break;
        }
    return out;
}

/* A file of a game's folder: by its title id, else by RomM's category. */
static nx_kind nx_file_kind(const char *name, const char *category) {
    unsigned long long game;
    nx_kind k = nx_title(name, &game, NULL);
    if (k != NX_NONE) return k;
    return !strcmp(category, "update") ? NX_UPDATE : !strcmp(category, "dlc") ? NX_DLC : NX_GAME;
}

/* The game a RomM entry is: the title id in its name, else the game's own
 * file's in its folder, else any file's. */
static nx_kind nx_rom(const cJSON *rom, unsigned long long *game) {
    nx_kind k = nx_title(jget_str(rom, "fs_name", ""), game, NULL);
    unsigned long long id;
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files")) {
        if (k == NX_GAME) break;
        const nx_kind fk = nx_title(jget_str(f, "file_name", ""), &id, NULL);
        if (fk != NX_NONE && (k == NX_NONE || fk == NX_GAME)) {
            k = fk;
            *game = id;
        }
    }
    return k;
}

static const char *nx_dir(const profile_map *m, nx_kind k) {
    return k == NX_UPDATE ? m->update_dir : k == NX_DLC ? m->dlc_dir : m->rom_dir;
}

/* The update and DLC RomM keeps as games of their own for a game, found by
 * the start of its title id: [{"id", "kind": "update" | "dlc"}], the newest
 * update only. */
typedef struct {
    cJSON *out;
    unsigned long long game;
    int platform_id;
    unsigned long newest;
} nx_find;

static int nx_add_addon(const cJSON *it, const cJSON *page, int first, void *ctx) {
    (void)page, (void)first;
    nx_find *fd = ctx;
    unsigned long long game;
    unsigned long version;
    if (!it || (int)jget_num(it, "platform_id", 0) != fd->platform_id) return 0;
    const nx_kind k = nx_title(jget_str(it, "fs_name", ""), &game, &version);
    if (game != fd->game || (k != NX_UPDATE && k != NX_DLC)) return 0;
    if (k == NX_UPDATE) {
        cJSON *have = NULL, *x;
        cJSON_ArrayForEach(x, fd->out) if (!strcmp(jget_str(x, "kind", ""), "update")) have = x;
        if (have && version <= fd->newest) return 0;
        if (have) cJSON_Delete(cJSON_DetachItemViaPointer(fd->out, have));
        fd->newest = version;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", jget_num(it, "id", 0));
    char tid[17];
    cJSON_AddStringToObject(o, "kind", k == NX_UPDATE ? "update" : "dlc");
    cJSON_AddStringToObject(o, "name", jget_str(it, "fs_name", ""));
    cJSON_AddStringToObject(o, "title_id", nx_title_hex(jget_str(it, "fs_name", ""), tid));
    cJSON_AddNumberToObject(o, "size", jget_num(it, "fs_size_bytes", 0));
    cJSON_AddItemToArray(fd->out, o);
    return 0;
}

static cJSON *nx_addons(int platform_id, unsigned long long game) {
    char path[256];
    snprintf(path, sizeof path, "/api/roms?platform_ids=%d&search_term=%012llX&order_by=name&order_dir=asc" ROMS_LEAN,
             platform_id, game >> 16);
    nx_find fd = {cJSON_CreateArray(), game, platform_id, 0};
    long st = 0;
    if (romm_list(path, 0, 500, nx_add_addon, &fd, &st) < 0) LOGW("could not look for the update and DLC of %016llX (HTTP %ld)", game, st);
    return fd.out;
}

/* GoodTools codes ("!", "M3", "hI", "t1", "b2", "a1", "f1", "o1", "p1", "T+Eng")
 * say nothing a player needs. */
static int readable_tag(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || !strcmp(t, "!")) return 0;
    if (n <= 3 && strchr("Mhtbafop", t[0])) {
        int code = 1;
        for (size_t i = 1; i < n; i++) code &= (t[i] >= '0' && t[i] <= '9') || (t[i] >= 'A' && t[i] <= 'Z') || t[i] == '+';
        if (code) return 0;
    }
    return !(t[0] == 'T' && (t[1] == '+' || t[1] == '-'));
}

static void add_list(char *buf, size_t size, const cJSON *arr, int max) {
    const cJSON *x;
    int n = 0;
    cJSON_ArrayForEach(x, arr) {
        if (!cJSON_IsString(x) || !readable_tag(x->valuestring) || n++ == max) continue;
        size_t l = strlen(buf);
        snprintf(buf + l, size - l, "%s%s", l ? " · " : "", x->valuestring);
    }
}

/* What sets one version of a game apart from the others RomM has for it
 * (its sibling_roms): "USA · Rev 1 · Disc 2". The extension when the file
 * names say nothing more. */
static void version_label(const cJSON *rom, char *buf, size_t size) {
    buf[0] = 0;
    add_list(buf, size, cJSON_GetObjectItemCaseSensitive(rom, "regions"), 2);
    const char *rev = jget_str(rom, "revision", "");
    if (rev[0]) {
        size_t l = strlen(buf);
        snprintf(buf + l, size - l, "%sRev %s", l ? " · " : "", rev);
    }
    add_list(buf, size, cJSON_GetObjectItemCaseSensitive(rom, "languages"), 3);
    add_list(buf, size, cJSON_GetObjectItemCaseSensitive(rom, "tags"), 2);
    if (!buf[0]) {
        const char *ext = jget_str(rom, "fs_extension", "");
        for (size_t i = 0; ext[i] && i + 1 < size; i++) buf[i] = (char)(ext[i] >= 'a' && ext[i] <= 'z' ? ext[i] - 32 : ext[i]), buf[i + 1] = 0;
    }
}

static void add_version(cJSON *o, const cJSON *rom) {
    const cJSON *sibs = cJSON_GetObjectItemCaseSensitive(rom, "sibling_roms"), *s;
    int others = cJSON_GetArraySize(sibs);
    if (others <= 0) return;
    char label[96];
    version_label(rom, label, sizeof label);
    cJSON_AddStringToObject(o, "version", label);
    cJSON_AddNumberToObject(o, "versions", others + 1);
    /* The other versions' ids: the game's card is on the console when any is. */
    cJSON *ids = cJSON_AddArrayToObject(o, "siblings");
    cJSON_ArrayForEach(s, sibs) cJSON_AddItemToArray(ids, cJSON_CreateNumber(jget_num(s, "id", 0)));
}

#define VERSIONS_MAX 12 /* versions listed in a game's details, this one included */
#define DISCS_MAX 16    /* discs counted in a game kept as a folder */

/* The disc a name says: "Final Fantasy IX (USA) (Disc 2) (Rev 1).chd" is 2;
 * 0 when it says none. */
static int disc_number(const char *name) {
    for (const char *p = name; *p; p++) {
        if (tolower((unsigned char)p[0]) != 'd' || tolower((unsigned char)p[1]) != 'i' ||
            tolower((unsigned char)p[2]) != 's' || tolower((unsigned char)p[3]) != 'c')
            continue;
        const char *d = p + 4;
        while (*d == ' ' || *d == '_' || *d == '-') d++;
        if (*d >= '1' && *d <= '9') return atoi(d);
    }
    return 0;
}

/* A game RomM keeps as a folder of discs: [{"disc", "size"}], a disc's files
 * (its .bin and .cue, its tracks) counted together. Only for two or more:
 * the folder still comes down whole, an emulator wants every disc. */
static void add_discs(cJSON *v, const cJSON *rom) {
    double size[DISCS_MAX + 1] = {0};
    char seen[DISCS_MAX + 1] = {0};
    int n = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files")) {
        const int d = disc_number(jget_str(f, "file_name", ""));
        if (d < 1 || d > DISCS_MAX || extra_file(f)) continue;
        n += !seen[d];
        seen[d] = 1;
        size[d] += jget_num(f, "file_size_bytes", 0);
    }
    if (n < 2) return;
    cJSON *arr = cJSON_AddArrayToObject(v, "discs");
    for (int d = 1; d <= DISCS_MAX; d++) {
        if (!seen[d]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "disc", d);
        cJSON_AddNumberToObject(o, "size", size[d]);
        cJSON_AddItemToArray(arr, o);
    }
}

static void version_entry(cJSON *arr, const cJSON *rom) {
    char label[96];
    version_label(rom, label, sizeof label);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddNumberToObject(v, "id", jget_num(rom, "id", 0));
    cJSON_AddStringToObject(v, "name", jget_str(rom, "fs_name_no_ext", jget_str(rom, "name", "")));
    cJSON_AddStringToObject(v, "label", label);
    cJSON_AddNumberToObject(v, "size", jget_num(rom, "fs_size_bytes", 0));
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rom, "has_multiple_files"))) add_discs(v, rom);
    cJSON_AddItemToArray(arr, v);
}

/* Every version of a game, this one first, for RommPS's picker:
 * [{"id", "name", "label", "size", "discs"}]. RomM's sibling entries carry no
 * size, tags or files, so each one is asked for on its own. None when there's
 * nothing to show: one version, not a folder of discs. */
static void add_version_list(cJSON *out, const cJSON *rom) {
    const cJSON *sibs = cJSON_GetObjectItemCaseSensitive(rom, "sibling_roms"), *s;
    cJSON *arr = cJSON_CreateArray();
    version_entry(arr, rom);
    int n = 1;
    cJSON_ArrayForEach(s, sibs) {
        if (n++ == VERSIONS_MAX) break;
        char path[64];
        snprintf(path, sizeof path, "/api/roms/%d", (int)jget_num(s, "id", 0));
        long st = 0;
        cJSON *sib = romm_call("GET", path, NULL, &st);
        /* A version whose file isn't on RomM's disk any more can't come down. */
        if (st == 200 && sib && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sib, "missing_from_fs")))
            version_entry(arr, sib);
        cJSON_Delete(sib);
    }
    if (cJSON_GetArraySize(arr) < 2 && !cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 0), "discs")) {
        cJSON_Delete(arr);
        return;
    }
    cJSON_AddItemToObject(out, "version_list", arr);
}

static void discard_partial(const char *dest, int multi) {
    char part[PATH_MAX_LEN];
    if (multi) {
        remove_tree(dest);
        return;
    }
    snprintf(part, sizeof part, "%s.part", dest);
    unlink(part);
}

static void record_rom(job *j, const cJSON *rom, const profile_map *m, const char *dest, int multi) {
    const char *fs_name = jget_str(rom, "fs_name", "");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files");
    char stem[256];
    cJSON *stems = cJSON_CreateArray();
    if (multi) {
        cJSON_AddItemToArray(stems, cJSON_CreateString(fs_name));
        const cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const char *fn = jget_str(f, "file_name", "");
            if (!ext_in_list(fn, ".cue,.m3u,.gdi,.chd,.ccd,.iso,.pbp")) continue;
            path_stem(stem, sizeof stem, fn);
            cJSON_AddItemToArray(stems, cJSON_CreateString(stem));
        }
    } else {
        path_stem(stem, sizeof stem, fs_name);
        cJSON_AddItemToArray(stems, cJSON_CreateString(stem));
    }
    state_lock();
    cJSON *e = state_entry_ensure("roms", dest);
    jset_num(e, "rom_id", j->target_id);
    jset_str(e, "profile", m->profile_id);
    jset_str(e, "platform_dir", m->platform_dir);
    jset_str(e, "name", jget_str(rom, "name", ""));
    jset_str(e, "fs_name", fs_name);
    jset_str(e, "md5", jget_str(rom, "md5_hash", ""));
    jset_num(e, "platform_id", jget_num(rom, "platform_id", 0));
    jset_str(e, "cover", jget_str(rom, "path_cover_small", ""));
    cJSON_DeleteItemFromObjectCaseSensitive(e, "miss_at");
    cJSON_DeleteItemFromObjectCaseSensitive(e, "stems");
    cJSON_AddItemToObject(e, "stems", stems);
    cJSON_DeleteItemFromObjectCaseSensitive(e, "discs");
    if (multi) {
        /* A folder of discs: each disc's files, so which ones are here can be
         * told when only some came down. Only its game files, extras kept or
         * not, so the list holds for every emulator the folder is copied to. */
        cJSON *discs = cJSON_CreateObject();
        const cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const int d = disc_number(jget_str(f, "file_name", ""));
            if (d < 1 || extra_file(f)) continue;
            char key[12];
            snprintf(key, sizeof key, "%d", d);
            cJSON *list = cJSON_GetObjectItemCaseSensitive(discs, key);
            if (!list) list = cJSON_AddArrayToObject(discs, key);
            cJSON_AddItemToArray(list, cJSON_CreateString(jget_str(f, "file_name", "")));
        }
        if (cJSON_GetArraySize(discs) > 1) cJSON_AddItemToObject(e, "discs", discs);
        else cJSON_Delete(discs);
    }
    state_save();
    state_unlock();
    installed_changed();
}

/* One file of a Switch game, recorded as RomM's game it is. An update or DLC
 * also says which game it came with ("with"): removing that game removes it,
 * and it isn't a game of its own on the console. title: the title id read
 * from inside it, when its name has none (0: none). */
static void record_file(int rom_id, const cJSON *rom, const profile_map *m, const char *dest, int with, uint64_t title) {
    char stem[256];
    path_stem(stem, sizeof stem, path_basename(dest));
    state_lock();
    cJSON *e = state_entry_ensure("roms", dest);
    jset_num(e, "rom_id", rom_id);
    jset_str(e, "profile", m->profile_id);
    jset_str(e, "platform_dir", m->platform_dir);
    jset_str(e, "name", jget_str(rom, "name", ""));
    jset_str(e, "fs_name", path_basename(dest));
    jset_num(e, "platform_id", jget_num(rom, "platform_id", 0));
    jset_str(e, "cover", jget_str(rom, "path_cover_small", ""));
    if (with > 0) jset_num(e, "with", with);
    cJSON_DeleteItemFromObjectCaseSensitive(e, "title_id");
    if (title) {
        char hex[20];
        snprintf(hex, sizeof hex, "%016llX", (unsigned long long)title);
        jset_str(e, "title_id", hex);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(e, "miss_at");
    cJSON_DeleteItemFromObjectCaseSensitive(e, "stems");
    cJSON *stems = cJSON_AddArrayToObject(e, "stems");
    cJSON_AddItemToArray(stems, cJSON_CreateString(stem));
    state_save();
    state_unlock();
    installed_changed();
}

/* After a game is down: the platform's BIOS files, then its saves, before it
 * shows as ready so it isn't started without them. game and m: the file and
 * its emulator, checked for a copy cut short on the server (NULL: not). */
static void finish_download(job *j, const profile_map *maps, int n, const cJSON *rom, const profile_map *m,
                            const char *game) {
    firmware_sync(maps, n, NULL, (int)jget_num(rom, "platform_id", 0), NULL);
    char problem[256] = "";
    if (m && game && game_file_problem(game, m->save_layout, problem, sizeof problem))
        LOGE("%s: %s", path_basename(game), problem);
    pthread_mutex_lock(&g_q_lock);
    j->status = JOB_SYNCING;
    pthread_mutex_unlock(&g_q_lock);
    char name[256], system[128], head[400];
    str_copy(name, sizeof name, j->name);
    str_copy(system, sizeof system, j->platform);
    sync_game_result saves;
    if (sync_game(j->target_id, name, system, problem[0] ? problem : NULL, &saves) != 0) {
        /* save sync off: just the game */
        config_lock();
        const int notify = g_cfg.notify;
        config_unlock();
        download_notice(head, sizeof head, name, system);
        if (notify && problem[0]) plat_notify("RommPS: %s, but %s", head, problem);
        else if (notify) plat_notify("RommPS: %s", head);
    }
    pthread_mutex_lock(&g_q_lock);
    j->saves = saves;
    pthread_mutex_unlock(&g_q_lock);
    job_end(j, JOB_DONE, NULL, NULL);
}

/* A file a Switch download brings: one of a game's, or of its update's or DLC's. */
typedef struct {
    int rom_id, file_id; /* file_id 0: the game's one file, by its name */
    int with;            /* the game an update or DLC came with, else 0 */
    nx_kind kind;
    int guessed;         /* a game for want of a word from its name or RomM: what's inside decides */
    uint64_t title;      /* the title id inside it, for a guessed one (0: not read, or none) */
    int had;             /* there, whole, before this download */
    int got;             /* brought by this download */
    int64_t size;
    char name[256];
    char dest[PATH_MAX_LEN];
    const cJSON *rom;
} nx_file;

static int nx_plan(const cJSON *rom, const profile_map *m, int with, nx_file *out, int max) {
    const char *fs_name = jget_str(rom, "fs_name", "");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files"), *f;
    int multi = cJSON_GetArraySize(files) > 0, n = 0;
    cJSON_ArrayForEach(f, files)
        if (!strcmp(jget_str(f, "file_name", ""), fs_name)) multi = 0;
    if (!multi) {
        if (max < 1) return 0;
        nx_file *x = &out[n++];
        memset(x, 0, sizeof *x);
        x->kind = nx_file_kind(fs_name, "");
        x->size = (int64_t)jget_num(rom, "fs_size_bytes", -1);
        str_copy(x->name, sizeof x->name, fs_name);
    } else {
        /* Every file lands flat in its folder: none is in a folder of the game's. */
        leave_rules lr = leave_rules_for(m, rom);
        lr.anywhere = 1;
        cJSON_ArrayForEach(f, files) {
            if (left_out(&lr, rom, f) || n == max) continue;
            nx_file *x = &out[n++];
            memset(x, 0, sizeof *x);
            x->file_id = (int)jget_num(f, "id", 0);
            x->kind = nx_file_kind(jget_str(f, "file_name", ""), jget_str(f, "category", ""));
            x->size = (int64_t)jget_num(f, "file_size_bytes", -1);
            str_copy(x->name, sizeof x->name, jget_str(f, "file_name", ""));
        }
    }
    for (int i = 0; i < n; i++) {
        unsigned long long named;
        out[i].rom_id = (int)jget_num(rom, "id", 0);
        out[i].with = with;
        out[i].rom = rom;
        out[i].guessed = out[i].kind == NX_GAME && nx_title(out[i].name, &named, NULL) == NX_NONE;
        path_join(out[i].dest, sizeof out[i].dest, nx_dir(m, out[i].kind), out[i].name);
        out[i].had = out[i].size >= 0 && file_size(out[i].dest) == out[i].size;
        /* Down before and found to be an update or DLC: it's where those go now. */
        for (int k = NX_UPDATE; out[i].guessed && !out[i].had && k <= NX_DLC; k++) {
            char there[PATH_MAX_LEN];
            path_join(there, sizeof there, nx_dir(m, (nx_kind)k), out[i].name);
            if (out[i].size < 0 || file_size(there) != out[i].size) continue;
            str_copy(out[i].dest, sizeof out[i].dest, there);
            out[i].kind = (nx_kind)k;
            out[i].had = 1;
        }
    }
    return n;
}

/* A file neither its name nor RomM said anything of ("Some Game
 * v3.0.3.nsp"): what's inside does (sync_nx_title). An update or DLC goes
 * where the emulator takes those, unless it was here before the download:
 * that one waits for the emulator to close (sync_nx_settle). */
static void nx_identify(nx_file *x, const profile_map *m) {
    if (!x->guessed) return;
    x->title = sync_nx_title(m, x->dest, 1);
    const switchid_kind k = switchid_kind_of(x->title);
    if (k != SWITCHID_UPDATE && k != SWITCHID_DLC) return;
    x->kind = k == SWITCHID_UPDATE ? NX_UPDATE : NX_DLC;
    char to[PATH_MAX_LEN];
    path_join(to, sizeof to, nx_dir(m, x->kind), x->name);
    int moved = 0;
    if (x->got && strcmp(to, x->dest) != 0 && !file_exists(to) && !dir_exists(to)) {
        moved = mkdir_p(nx_dir(m, x->kind)) == 0 && move_file(x->dest, to) == 0;
        if (moved) str_copy(x->dest, sizeof x->dest, to);
        else LOGW("could not move %s to %s", x->name, nx_dir(m, x->kind));
    }
    LOGI("%s is %s of %016llX, not a game of its own%s%s", x->name, k == SWITCHID_UPDATE ? "an update" : "DLC",
         (unsigned long long)switchid_game_of(x->title), moved ? ": in " : "", moved ? nx_dir(m, x->kind) : "");
}

static void run_switch_job(job *j, const cJSON *rom, const profile_map *maps, int n, const profile_map *m) {
    nx_file *files = calloc(NX_MAX_FILES, sizeof *files);
    cJSON *addons = cJSON_CreateArray(), *a;
    if (!files) {
        job_end(j, JOB_ERROR, "out of memory%s", "");
        cJSON_Delete(addons);
        return;
    }
    int nf = nx_plan(rom, m, 0, files, NX_MAX_FILES), skipped = 0;
    unsigned long long game;
    if (nx_rom(rom, &game) == NX_GAME) {
        cJSON *found = nx_addons((int)jget_num(rom, "platform_id", 0), game);
        cJSON_ArrayForEach(a, found) {
            char path[64];
            snprintf(path, sizeof path, "/api/roms/%d", (int)jget_num(a, "id", 0));
            long st = 0;
            cJSON *full = romm_call("GET", path, NULL, &st);
            if (st != 200 || !full) {
                LOGW("%s: could not load its %s (RomM game %d)", j->name, jget_str(a, "kind", ""), (int)jget_num(a, "id", 0));
                cJSON_Delete(full);
                continue;
            }
            if (rom_archive(full, 1)) {
                LOGW("%s: its %s %s is archived, left out", j->name, jget_str(a, "kind", ""), jget_str(full, "fs_name", ""));
                cJSON_Delete(full);
                skipped++;
                continue;
            }
            nf += nx_plan(full, m, j->target_id, files + nf, NX_MAX_FILES - nf);
            cJSON_AddItemToArray(addons, full); /* the plan points into it */
        }
        cJSON_Delete(found);
    }
    int64_t total = 0;
    for (int i = 0; i < nf; i++) total += files[i].size > 0 ? files[i].size : 0;
    pthread_mutex_lock(&g_q_lock);
    j->total = total;
    pthread_mutex_unlock(&g_q_lock);

    int ok = 1, ups = 0, dlcs = 0;
    int64_t base = 0;
    http_resp r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < nf && ok; i++) {
        nx_file *x = &files[i];
        if (x->had) {
            base += x->size;
            continue;
        }
        char path[2048];
        char *q = http_escape(x->name);
        if (x->file_id) snprintf(path, sizeof path, "/api/roms/%d/files/content/%s", x->file_id, q);
        else snprintf(path, sizeof path, "/api/roms/%d/content/%s", x->rom_id, q);
        free(q);
        mkdir_p(nx_dir(m, x->kind));
        pthread_mutex_lock(&g_q_lock);
        j->base = base;
        snprintf(j->note, sizeof j->note, "%s", x->kind == NX_UPDATE ? "Update" : x->kind == NX_DLC ? "DLC" : "");
        pthread_mutex_unlock(&g_q_lock);
        x->got = romm_download(path, x->dest, progress_cb, j, &r) == 0;
        ok = x->got && !cancelled(j);
        base += file_size(x->dest) > 0 ? file_size(x->dest) : 0;
    }
    if (ok) {
        for (int i = 0; i < nf; i++) {
            nx_identify(&files[i], m);
            ups += files[i].kind == NX_UPDATE;
            dlcs += files[i].kind == NX_DLC;
            record_file(files[i].rom_id, files[i].rom, m, files[i].dest, files[i].with, files[i].title);
        }
        for (int i = 0; i < nf; i++) sync_nx_link(m, files[i].dest);
        LOGI("downloaded %s: %d file%s, %d update, %d DLC%s", j->name, nf, nf == 1 ? "" : "s", ups, dlcs,
             skipped ? " (archived ones left out)" : "");
        /* Another Switch emulator with folders of its own gets the same files. */
        const char *slug = jget_str(rom, "platform_slug", "");
        for (int k = 0; k < n; k++) {
            const profile_map *x = &maps[k];
            if (x == m || !x->extra || x->twin || !profile_map_handles(x, slug, jget_str(rom, "platform_fs_slug", ""))) continue;
            for (int i = 0; i < nf; i++) {
                char other[PATH_MAX_LEN];
                path_join(other, sizeof other, nx_dir(x, files[i].kind), files[i].name);
                if (!strcmp(other, files[i].dest)) continue;
                mkdir_p(nx_dir(x, files[i].kind));
                if (link_or_copy(files[i].dest, other) == 0) {
                    record_file(files[i].rom_id, files[i].rom, x, other, files[i].with, files[i].title);
                    sync_nx_link(x, other);
                } else {
                    LOGW("could not put %s in %s", files[i].name, nx_dir(x, files[i].kind));
                }
            }
        }
        finish_download(j, maps, n, rom, NULL, NULL);
    } else {
        /* Cancelled: what this download brought goes; what was there stays. */
        for (int i = 0; i < nf; i++) {
            char part[PATH_MAX_LEN + 8];
            snprintf(part, sizeof part, "%s.part", files[i].dest);
            unlink(part);
            if (files[i].got && cancelled(j)) unlink(files[i].dest);
        }
        job_end(j, JOB_ERROR, "%s", r.error);
    }
    cJSON_Delete(addons);
    free(files);
}

/* Another disc than the one asked for: left on RomM. Files no disc names
 * (the .m3u) come with any disc. */
static int other_disc(const job *j, const cJSON *f) {
    const int d = disc_number(jget_str(f, "file_name", ""));
    return j->disc && d && d != j->disc;
}

/* The folder a game goes in: rom_dir, unless it's already in one the emulator
 * still plays from (old_rom_dirs). Downloaded again, it's replaced there
 * rather than turning up twice. */
static const char *games_dir(const profile_map *m, const char *name, char *buf, size_t n) {
    char list[sizeof m->old_rom_dirs], path[PATH_MAX_LEN], *save = NULL;
    path_join(path, sizeof path, m->rom_dir, name);
    if (file_exists(path) || dir_exists(path)) return m->rom_dir;
    str_copy(list, sizeof list, m->old_rom_dirs);
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        path_join(path, sizeof path, t, name);
        if (file_exists(path) || dir_exists(path)) {
            str_copy(buf, n, t);
            return buf;
        }
    }
    return m->rom_dir;
}

static void run_rom_job(job *j) {
    char path[2048], dest[PATH_MAX_LEN];
    snprintf(path, sizeof path, "/api/roms/%d", j->target_id);
    long st = 0;
    cJSON *rom = romm_call("GET", path, NULL, &st);
    if (st != 200 || !rom) {
        job_end(j, JOB_ERROR, "could not load the game from RomM%s", "");
        cJSON_Delete(rom);
        return;
    }
    pthread_mutex_lock(&g_q_lock);
    str_copy(j->name, sizeof j->name, jget_str(rom, "name", jget_str(rom, "fs_name", "?")));
    if (j->disc) {
        const size_t l = strlen(j->name);
        snprintf(j->name + l, sizeof j->name - l, " (Disc %d)", j->disc);
    }
    str_copy(j->cover, sizeof j->cover, jget_str(rom, "path_cover_small", ""));
    str_copy(j->platform, sizeof j->platform, console_name(rom, "platform_slug", "platform_fs_slug", "platform_display_name"));
    j->total = (int64_t)jget_num(rom, "fs_size_bytes", 0);
    pthread_mutex_unlock(&g_q_lock);

    profile_map *maps = NULL;
    int n = load_maps(&maps);
    const char *slug = jget_str(rom, "platform_slug", "");
    const profile_map *m = profile_find_for_platform(maps, n, slug, jget_str(rom, "platform_fs_slug", ""));
    if (!m) {
        if (maps_failed(maps, n)) job_end(j, JOB_ERROR, MAPS_SHORT_MSG "%s", "");
        else job_end(j, JOB_ERROR, "no emulator is set up for %s", slug);
        profiles_free(maps);
        cJSON_Delete(rom);
        return;
    }

    const char *fs_name = jget_str(rom, "fs_name", "");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files");
    /* fs_name is one of files[] only for a plain single-file rom; otherwise
     * it's the folder's name (no extension) and the rom is fetched per file. */
    int multi = cJSON_GetArraySize(files) > 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, files)
        if (strcmp(jget_str(f, "file_name", ""), fs_name) == 0) { multi = 0; break; }
    if (library_not_game(fs_name)) {
        job_end(j, JOB_ERROR, "%s isn't a game", fs_name);
        profiles_free(maps);
        cJSON_Delete(rom);
        return;
    }
    const char *archive = rom_archive(rom, is_switch(slug));
    if (archive) {
        job_end(j, JOB_ERROR, ARCHIVED_MSG, archive);
        profiles_free(maps);
        cJSON_Delete(rom);
        return;
    }
    if (is_switch(slug)) {
        run_switch_job(j, rom, maps, n, m);
        profiles_free(maps);
        cJSON_Delete(rom);
        return;
    }
    const leave_rules lr = leave_rules_for(m, rom);
    if (multi) {
        const cJSON *f;
        int64_t skipped = 0;
        cJSON_ArrayForEach(f, files) if (left_out(&lr, rom, f) || other_disc(j, f)) skipped += (int64_t)jget_num(f, "file_size_bytes", 0);
        pthread_mutex_lock(&g_q_lock);
        if (skipped < j->total) j->total -= skipped;
        pthread_mutex_unlock(&g_q_lock);
    }
    /* The game is dest, or with game_folders a folder of its own holding it. */
    char game[PATH_MAX_LEN], stem[256], into[PATH_MAX_LEN];
    const int wrap = !multi && m->game_folders;
    path_stem(stem, sizeof stem, fs_name);
    const char *dir = games_dir(m, wrap ? stem : fs_name, into, sizeof into);
    if (wrap) {
        path_join(game, sizeof game, dir, stem);
        path_join(dest, sizeof dest, game, fs_name);
    } else {
        path_join(dest, sizeof dest, dir, fs_name);
        str_copy(game, sizeof game, dest);
    }
    mkdir_p(wrap ? game : dir);
    /* A game downloaded again: cancelling keeps what was already there. */
    const int had = file_exists(dest) || dir_exists(dest);
    int ok = 1;
    http_resp r;
    if (!multi) {
        char *q = http_escape(fs_name);
        snprintf(path, sizeof path, "/api/roms/%d/content/%s", j->target_id, q);
        free(q);
        ok = romm_download(path, dest, progress_cb, j, &r) == 0;
    } else {
        /* Fetched file by file into <rom_dir>/<fs_name>/ so nothing needs
         * unzipping, each in its folder within the game (game_subdir). */
        int64_t base = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const char *fn = jget_str(f, "file_name", ""), *in = game_subdir(rom, f);
            if (left_out(&lr, rom, f) || other_disc(j, f)) continue;
            char fdest[PATH_MAX_LEN], sub[PATH_MAX_LEN];
            if (in[0]) {
                path_join(sub, sizeof sub, dest, in);
                path_join(fdest, sizeof fdest, sub, fn);
                mkdir_p(sub);
            } else {
                path_join(fdest, sizeof fdest, dest, fn);
            }
            int64_t size = (int64_t)jget_num(f, "file_size_bytes", -1);
            if (file_size(fdest) == size) {
                base += size;
                continue;
            }
            char *q = http_escape(fn);
            snprintf(path, sizeof path, "/api/roms/%d/files/content/%s", (int)jget_num(f, "id", 0), q);
            free(q);
            pthread_mutex_lock(&g_q_lock);
            j->base = base;
            pthread_mutex_unlock(&g_q_lock);
            if (romm_download(path, fdest, progress_cb, j, &r) != 0) {
                ok = 0;
                break;
            }
            base += file_size(fdest);
        }
    }

    if (ok && !cancelled(j)) {
        record_rom(j, rom, m, game, multi);
        LOGI("downloaded %s to %s", j->name, game);
        /* The extra emulators for this system get the same game: a hard link
         * where they share a drive, a copy where they don't. */
        for (int i = 0; i < n; i++) {
            const profile_map *x = &maps[i];
            if (x == m || !x->extra || x->twin || !profile_map_handles(x, slug, jget_str(rom, "platform_fs_slug", ""))) continue;
            int covered = plat_is_mount(x->rom_dir);
            for (int k = 0; k < n && !covered; k++) covered = maps[k].mount_title[0] && !strcmp(maps[k].own_rom_dir, x->rom_dir);
            if (covered) continue; /* an extra's mount shows it the game */
            char other[PATH_MAX_LEN];
            path_join(other, sizeof other, x->rom_dir, path_basename(game));
            if (!strcmp(other, game)) continue;
            mkdir_p(x->rom_dir);
            pthread_mutex_lock(&g_q_lock);
            snprintf(j->note, sizeof j->note, "Copying to %s", x->profile_name);
            j->base = j->done = j->total = 0;
            j->speed = 0;
            j->sample_t = 0;
            pthread_mutex_unlock(&g_q_lock);
            if (link_or_copy_progress(game, other, progress_cb, j) == 0) {
                record_rom(j, rom, x, other, multi);
                LOGI("also for %s: %s", x->profile_name, other);
            } else {
                LOGW("could not put %s in %s", j->name, x->rom_dir);
            }
        }
        finish_download(j, maps, n, rom, m, game);
    } else {
        if (cancelled(j) && !(multi && had)) discard_partial(dest, multi);
        if (wrap && !had) rmdir(game); /* only when empty */
        job_end(j, JOB_ERROR, "%s", r.error);
    }
    profiles_free(maps);
    cJSON_Delete(rom);
}

static void run_firmware_job(job *j) {
    char path[64];
    profile_map *maps = NULL;
    int n = load_maps(&maps), failed = 0;
    long st = 0;
    snprintf(path, sizeof path, "/api/platforms/%d", j->target_id);
    cJSON *plat = romm_call("GET", path, NULL, &st);
    pthread_mutex_lock(&g_q_lock);
    str_copy(j->platform, sizeof j->platform, console_name(plat, "slug", "fs_slug", jget_str(plat, "display_name", "")[0] ? "display_name" : "name"));
    pthread_mutex_unlock(&g_q_lock);
    if (st != 200 || !profile_find_for_platform(maps, n, jget_str(plat, "slug", ""), jget_str(plat, "fs_slug", ""))) {
        if (st == 200 && maps_failed(maps, n)) job_end(j, JOB_ERROR, MAPS_SHORT_MSG "%s", "");
        else job_end(j, JOB_ERROR, "no emulator is set up for %s", plat ? jget_str(plat, "slug", "") : "this platform");
        cJSON_Delete(plat);
        profiles_free(maps);
        return;
    }
    /* Every emulator that plays it gets them, as a sync does. */
    cJSON *one = cJSON_CreateArray();
    cJSON_AddItemToArray(one, plat);
    int count = firmware_sync(maps, n, one, j->target_id, &failed);
    cJSON_Delete(one);
    pthread_mutex_lock(&g_q_lock);
    snprintf(j->name, sizeof j->name, "BIOS files (%d new)", count > 0 ? count : 0);
    pthread_mutex_unlock(&g_q_lock);
    char msg[32];
    snprintf(msg, sizeof msg, "%d file(s) failed", failed);
    job_end(j, failed || count < 0 ? JOB_ERROR : JOB_DONE, "%s", msg);
    profiles_free(maps);
}

static int concurrency(void) {
    config_lock();
    int n = g_cfg.download_concurrency;
    config_unlock();
    return n < 1 ? 1 : n > MAX_WORKERS ? MAX_WORKERS : n;
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_q_lock);
        job *j;
        while (g_running >= concurrency() || !(j = oldest_queued())) pthread_cond_wait(&g_q_cond, &g_q_lock);
        j->status = JOB_RUNNING;
        g_running++;
        pthread_mutex_unlock(&g_q_lock);

        if (j->type == JOB_ROM)
            run_rom_job(j);
        else
            run_firmware_job(j);

        pthread_mutex_lock(&g_q_lock);
        g_running--;
        pthread_cond_broadcast(&g_q_cond);
        pthread_mutex_unlock(&g_q_lock);
    }
    return NULL;
}

/* Back in the queue as it was first asked for. Caller holds the lock, and
 * checked the game isn't already downloading. */
static void job_requeue(job *j) {
    j->status = JOB_QUEUED;
    j->cancel = j->when_back = 0;
    j->error[0] = j->note[0] = 0;
    j->done = j->total = j->base = j->sample_bytes = 0;
    j->speed = j->sample_t = 0;
    memset(&j->saves, 0, sizeof j->saves);
}

/* Whether a job fetches what another does: the same game, unless they're
 * different discs of it. */
static int same_files(const job *p, job_type type, int id, int disc) {
    return p->type == type && p->target_id == id && (!p->disc || !disc || p->disc == disc);
}

static int downloading(const job *j) {
    for (const job *p = g_jobs; p; p = p->next)
        if (p != j && same_files(p, j->type, j->target_id, j->disc) && !finished(p)) return 1;
    return 0;
}

/* RomM answers again: what failed for want of it goes again. */
static void romm_back(const char *origin) {
    int n = 0;
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j; j = j->next)
        if (j->status == JOB_ERROR && j->when_back && !downloading(j)) {
            job_requeue(j);
            n++;
        }
    if (n) pthread_cond_broadcast(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
    if (n) LOGI("%s answers again: restarting %d download%s", origin, n, n == 1 ? "" : "s");
}

/* What a download or a copy left in dir and the folders in it when the
 * payload stopped under it (an update, a crash, the console going off):
 * .part and .tmp files, and a folder only they were in. Nothing is
 * downloading or copying yet at startup, so none are anyone's. */
static int clear_parts_in(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int gone = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        if (depth > 0 && dir_exists(path) && !plat_is_mount(path)) {
            const int here = clear_parts_in(path, depth - 1);
            if (here && rmdir(path) == 0) LOGI("removed %s, a download or copy that didn't finish", path);
            gone += here;
        } else if (ext_in_list(de->d_name, ".part,.tmp") && unlink(path) == 0) {
            LOGI("removed %s, a download or copy that didn't finish", path);
            gone++;
        }
    }
    closedir(d);
    return gone;
}

static void clear_parts(void) {
    profile_map *maps = NULL;
    int n = load_maps(&maps);
    for (int i = 0; i < n; i++) {
        const char *dirs[] = {maps[i].own_rom_dir, maps[i].rom_dir, maps[i].update_dir, maps[i].dlc_dir};
        for (size_t k = 0; k < sizeof dirs / sizeof dirs[0]; k++) {
            int seen = 0;
            for (size_t j = 0; j < k; j++) seen |= !strcmp(dirs[j], dirs[k]);
            if (dirs[k][0] && !seen && !plat_is_mount(dirs[k])) clear_parts_in(dirs[k], 1);
        }
        /* A game downloaded again where it was (games_dir) can leave some there too. */
        char list[sizeof maps[i].old_rom_dirs], *save = NULL;
        str_copy(list, sizeof list, maps[i].old_rom_dirs);
        for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save))
            if (strcmp(t, maps[i].rom_dir) != 0 && !plat_is_mount(t)) clear_parts_in(t, 1);
    }
    profiles_free(maps);
}

void library_init(void) {
    http_on_back(romm_back);
    clear_parts();
    /* A copy: comparing old copies can take a while, and nothing waits on it. */
    config_lock();
    cJSON *profiles = cJSON_Duplicate(g_cfg.profiles, 1);
    config_unlock();
    profiles_follow_moves(profiles);
    cJSON_Delete(profiles);
    for (int i = 0; i < MAX_WORKERS; i++) {
        thread_start(worker, NULL);
    }
}

int library_busy(void) {
    int busy = 0;
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j && !busy; j = j->next)
        busy = j->status == JOB_QUEUED || j->status == JOB_RUNNING || j->status == JOB_SYNCING;
    pthread_mutex_unlock(&g_q_lock);
    return busy;
}

void library_wake(void) {
    pthread_mutex_lock(&g_q_lock);
    pthread_cond_broadcast(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
}

/* Drops the oldest finished jobs once the list is long. Caller holds the lock. */
static void trim_jobs(void) {
    int n = 0;
    for (job *p = g_jobs; p; p = p->next) n++;
    while (n > MAX_JOBS) {
        job **pp = &g_jobs, **oldest = NULL;
        for (; *pp; pp = &(*pp)->next)
            if (finished(*pp)) oldest = pp;
        if (!oldest) break;
        job *dead = *oldest;
        *oldest = dead->next;
        free(dead);
        n--;
    }
}

static int enqueue(job_type type, int id, int disc, const char *name, const char *cover, char *err, int en) {
    pthread_mutex_lock(&g_q_lock);
    for (job *p = g_jobs; p; p = p->next) {
        if (same_files(p, type, id, disc) && !finished(p)) {
            pthread_mutex_unlock(&g_q_lock);
            snprintf(err, (size_t)en, "already downloading");
            return -1;
        }
    }
    job *j = calloc(1, sizeof *j);
    if (!j) {
        pthread_mutex_unlock(&g_q_lock);
        snprintf(err, (size_t)en, "out of memory");
        return -1;
    }
    j->type = type;
    j->target_id = id;
    j->disc = disc;
    str_copy(j->name, sizeof j->name, name);
    str_copy(j->cover, sizeof j->cover, cover ? cover : "");
    j->id = ++g_job_seq;
    j->next = g_jobs;
    g_jobs = j;
    trim_jobs();
    pthread_cond_broadcast(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
    return 0;
}

int library_queue_rom(int rom_id, int disc, const char *name, const char *cover, char *err, int en) {
    /* The name the client shows, so the download is named from the start;
     * RomM's own replaces it once the job runs. */
    char fallback[32];
    snprintf(fallback, sizeof fallback, "Game #%d", rom_id);
    /* An archive is turned down at once, so the client can say why; RomM
     * out of reach leaves that to the download. */
    char path[64];
    snprintf(path, sizeof path, "/api/roms/%d", rom_id);
    long st = 0;
    cJSON *rom = romm_call("GET", path, NULL, &st);
    const char *archive = st == 200 ? rom_archive(rom, is_switch(jget_str(rom, "platform_slug", ""))) : NULL;
    const int refused = archive != NULL;
    if (refused) snprintf(err, (size_t)en, ARCHIVED_MSG, archive);
    cJSON_Delete(rom);
    if (refused) return -1;
    return enqueue(JOB_ROM, rom_id, disc > 0 ? disc : 0, name && name[0] ? name : fallback, cover, err, en);
}

int library_queue_scope(const library_scope *scope, int *queued, int *skipped, char *err, int en) {
    *queued = *skipped = 0;
    /* Games of a platform nothing here plays would only fail in the queue. */
    cJSON *plats = library_platforms(err, en);
    if (!plats) return -1;
    int played[256], np = 0;
    const cJSON *p, *it;
    cJSON_ArrayForEach(p, plats)
        if (np < (int)(sizeof played / sizeof played[0]) && jget_str(p, "profile", "")[0])
            played[np++] = (int)jget_num(p, "id", 0);
    cJSON_Delete(plats);
    for (int offset = 0, total = 1; offset < total;) {
        cJSON *page = library_roms(scope, "", offset, 500, err, en);
        if (!page) return -1;
        total = (int)jget_num(page, "total", 0);
        int got = 0;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(page, "items")) {
            got++;
            const int plat = (int)jget_num(it, "platform_id", 0);
            int ok = 0;
            for (int k = 0; k < np && !ok; k++) ok = played[k] == plat;
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "not_game"))) continue;
            if (!ok || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "installed")) ||
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "archived"))) {
                ++*skipped;
                continue;
            }
            char e2[160];
            if (enqueue(JOB_ROM, (int)jget_num(it, "id", 0), 0, jget_str(it, "name", ""), jget_str(it, "cover", ""), e2,
                        sizeof e2) == 0)
                ++*queued;
            else
                ++*skipped; /* already downloading */
        }
        cJSON_Delete(page);
        if (!got) break;
        offset += got;
    }
    return 0;
}

int library_queue_firmware(int platform_id, char *err, int en) {
    return enqueue(JOB_FIRMWARE, platform_id, 0, "BIOS files", NULL, err, en);
}

int library_cancel(int id, char *err, int en) {
    int rc = -1;
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j; j = j->next) {
        if (j->id != id) continue;
        if (j->status == JOB_QUEUED) {
            j->status = JOB_CANCELLED;
            rc = 0;
        } else if (j->status == JOB_RUNNING) {
            j->cancel = 1; /* the progress callback aborts the transfer */
            rc = 0;
        } else if (j->status == JOB_SYNCING) {
            snprintf(err, (size_t)en, "the game is downloaded, its saves are syncing");
        } else {
            snprintf(err, (size_t)en, "download already finished");
        }
        break;
    }
    pthread_mutex_unlock(&g_q_lock);
    if (rc && !err[0]) snprintf(err, (size_t)en, "no such download");
    return rc;
}

int library_retry(int id, char *err, int en) {
    int rc = -1;
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j; j = j->next) {
        if (j->id != id) continue;
        if (j->status != JOB_ERROR && j->status != JOB_CANCELLED)
            snprintf(err, (size_t)en, finished(j) ? "that download finished" : "that download is still going");
        else if (downloading(j))
            snprintf(err, (size_t)en, "already downloading");
        else {
            job_requeue(j);
            pthread_cond_broadcast(&g_q_cond);
            rc = 0;
        }
        break;
    }
    pthread_mutex_unlock(&g_q_lock);
    if (rc && !err[0]) snprintf(err, (size_t)en, "no such download");
    return rc;
}

int library_clear_finished(void) {
    int n = 0;
    pthread_mutex_lock(&g_q_lock);
    job **pp = &g_jobs;
    while (*pp) {
        if (finished(*pp)) {
            job *dead = *pp;
            *pp = dead->next;
            free(dead);
            n++;
        } else {
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_q_lock);
    return n;
}

/* A game that's already gone (deleted by hand, or its drive unplugged) counts
 * as removed: there's nothing left to fail on. */
int library_delete_rom(int rom_id, char *err, int en) {
    /* 0 is what files RomM didn't match are recorded as: never theirs to delete. */
    if (rom_id <= 0) {
        snprintf(err, (size_t)en, "no game given");
        return -1;
    }
    int removed = 0;
    state_lock();
    cJSON *roms = state_section("roms"), *e = roms ? roms->child : NULL;
    while (e) {
        cJSON *next = e->next;
        /* A Switch game's update and DLC go with it. */
        if ((int)jget_num(e, "rom_id", -1) == rom_id || (int)jget_num(e, "with", 0) == rom_id) {
            LOGI("removing %s (saves are kept)", e->string);
            remove_tree(e->string);
            cJSON_DeleteItemFromObjectCaseSensitive(roms, e->string);
            removed++;
        }
        e = next;
    }
    state_save();
    state_unlock();
    installed_changed();
    if (!removed) LOGI("rom %d: nothing of it on the console to remove", rom_id);
    /* Can't leave a finished download in the downloads page if we removed that title. */
    pthread_mutex_lock(&g_q_lock);
    for (job **pp = &g_jobs; *pp;) {
        job *dead = *pp;
        if (dead->type == JOB_ROM && dead->target_id == rom_id && finished(dead)) {
            *pp = dead->next;
            free(dead);
        } else {
            pp = &dead->next;
        }
    }
    pthread_mutex_unlock(&g_q_lock);
    return 0;
}

int library_delete_disc(int rom_id, int disc, char *err, int en) {
    char key[12];
    snprintf(key, sizeof key, "%d", disc);
    int found = 0, left = 0;
    state_lock();
    const cJSON *e, *d, *f;
    cJSON_ArrayForEach(e, state_section("roms")) {
        const cJSON *discs = cJSON_GetObjectItemCaseSensitive(e, "discs");
        if ((int)jget_num(e, "rom_id", -1) != rom_id || !cJSON_IsObject(discs)) continue;
        cJSON_ArrayForEach(d, discs) {
            const int this_one = !strcmp(d->string, key);
            found |= this_one;
            cJSON_ArrayForEach(f, d) {
                char path[PATH_MAX_LEN];
                path_join(path, sizeof path, e->string, cJSON_IsString(f) ? f->valuestring : "");
                if (!this_one) {
                    left |= file_exists(path);
                } else if (file_exists(path)) {
                    LOGI("removing %s (disc %d; saves are kept)", path, disc);
                    unlink(path);
                }
            }
        }
    }
    state_unlock();
    if (!found) {
        snprintf(err, (size_t)en, "no disc %d of that game here", disc);
        return -1;
    }
    if (!left) return library_delete_rom(rom_id, err, en); /* the last disc: the folder goes too */
    installed_changed();
    return 0;
}

cJSON *library_downloads(void) {
    static const char *names[] = {"queued", "downloading", "done", "error", "cancelled", "syncing"};
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j; j = j->next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", j->id);
        cJSON_AddStringToObject(o, "type", j->type == JOB_ROM ? "rom" : "firmware");
        cJSON_AddNumberToObject(o, j->type == JOB_ROM ? "rom_id" : "platform_id", j->target_id);
        if (j->disc) cJSON_AddNumberToObject(o, "disc", j->disc);
        cJSON_AddStringToObject(o, "name", j->name);
        cJSON_AddStringToObject(o, "platform", j->platform);
        if (j->type == JOB_ROM) cJSON_AddStringToObject(o, "cover", j->cover);
        cJSON_AddStringToObject(o, "status", j->status == JOB_RUNNING && j->cancel ? "cancelling" : names[j->status]);
        cJSON_AddStringToObject(o, "error", j->error);
        cJSON_AddNumberToObject(o, "done", (double)j->done);
        cJSON_AddNumberToObject(o, "total", (double)j->total);
        cJSON_AddNumberToObject(o, "speed", j->status == JOB_RUNNING ? j->speed : 0);
        cJSON_AddStringToObject(o, "note", j->status == JOB_RUNNING ? j->note : "");
        if (j->status == JOB_DONE && j->type == JOB_ROM) {
            cJSON_AddNumberToObject(o, "saves", j->saves.saves);
            cJSON_AddNumberToObject(o, "states", j->saves.states);
            cJSON_AddNumberToObject(o, "save_conflicts", j->saves.conflicts);
            cJSON_AddBoolToObject(o, "saves_failed", j->saves.failed);
        }
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&g_q_lock);
    return arr;
}

cJSON *library_platforms(char *err, int en) {
    long st = 0;
    cJSON *plats = romm_call("GET", "/api/platforms", NULL, &st);
    if (st != 200 || !cJSON_IsArray(plats)) {
        if (st || !romm_no_answer(err, en)) snprintf(err, (size_t)en, "could not load platforms (HTTP %ld)", st);
        cJSON_Delete(plats);
        return NULL;
    }
    profile_map *maps = NULL;
    int n = load_maps(&maps);
    cJSON *out = cJSON_CreateArray();
    const cJSON *p;
    cJSON_ArrayForEach(p, plats) {
        if (jget_num(p, "rom_count", 0) <= 0) continue;
        const profile_map *m = profile_find_for_platform(maps, n, jget_str(p, "slug", ""), jget_str(p, "fs_slug", ""));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", jget_num(p, "id", 0));
        cJSON_AddStringToObject(o, "slug", jget_str(p, "slug", ""));
        cJSON_AddStringToObject(o, "fs_slug", jget_str(p, "fs_slug", ""));
        cJSON_AddStringToObject(o, "name", console_name(p, "slug", "fs_slug", jget_str(p, "display_name", "")[0] ? "display_name" : "name"));
        cJSON_AddNumberToObject(o, "rom_count", jget_num(p, "rom_count", 0));
        cJSON_AddNumberToObject(o, "firmware_count", jget_num(p, "firmware_count", 0));
        if (m) {
            cJSON_AddStringToObject(o, "profile", m->profile_name);
            cJSON_AddStringToObject(o, "rom_dir", m->rom_dir);
        }
        cJSON_AddItemToArray(out, o);
    }
    profiles_free(maps);
    cJSON_Delete(plats);
    return out;
}

/* RomM's collection kinds: where it lists them, and what /api/roms takes to
 * list one's games. Virtual ones come grouped by a type (genre, franchise...). */
static const struct {
    const char *name, *list_path, *rom_param;
    int numeric_ids, offered;
} g_collection_kinds[] = {
    [COLLECTION_REGULAR] = {"regular", "/api/collections", "collection_id", 1, 1},
    [COLLECTION_SMART] = {"smart", "/api/collections/smart", "smart_collection_id", 1, 1},
    [COLLECTION_VIRTUAL] = {"virtual", "/api/collections/virtual?type=all", "virtual_collection_id", 0, 1},
};
#define COLLECTION_KINDS ((int)(sizeof g_collection_kinds / sizeof g_collection_kinds[0]))

/* What a collection carries that nothing here reads: every game's id and
 * large cover, about 200 bytes a game. Its small covers stay, for the first. */
static const char *const COLLECTION_DROP[] = {"rom_ids", "path_covers_large", NULL};

static int collection_offered(collection_kind kind) {
    return kind >= 0 && (int)kind < COLLECTION_KINDS && g_collection_kinds[kind].offered;
}

collection_kind library_collection_kind(const char *name) {
    for (int k = 0; k < COLLECTION_KINDS; k++)
        if (name && !strcmp(name, g_collection_kinds[k].name) && collection_offered((collection_kind)k))
            return (collection_kind)k;
    return COLLECTION_NONE;
}

int library_collection_id_ok(collection_kind kind, const char *id) {
    if (!collection_offered(kind) || !id || !id[0]) return 0;
    if (!g_collection_kinds[kind].numeric_ids) return 1;
    if (strlen(id) > 9) return 0;
    for (const char *c = id; *c; c++)
        if (!isdigit((unsigned char)*c)) return 0;
    return atoi(id) > 0;
}

/* Where a virtual collection's type sorts: RomM's known ones in this order,
 * any other after them. */
static int virtual_type_order(const char *type) {
    static const char *const order[] = {"franchise", "collection", "genre", "company", "mode", NULL};
    for (int i = 0; order[i]; i++)
        if (!strcmp(type, order[i])) return i;
    return 99;
}

/* In sections, as clients show them: regular, smart, then virtual ones by
 * type; in each the user's own first, then by name. */
static int collection_order(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    int c = library_collection_kind(jget_str(x, "kind", "")) - library_collection_kind(jget_str(y, "kind", ""));
    if (c) return c;
    const char *xt = jget_str(x, "type", ""), *yt = jget_str(y, "type", "");
    if ((c = virtual_type_order(xt) - virtual_type_order(yt)) || (c = strcmp(xt, yt))) return c;
    int xo = !jget_str(x, "owner", "")[0], yo = !jget_str(y, "owner", "")[0];
    if (xo != yo) return yo - xo;
    return strcasecmp(jget_str(x, "name", ""), jget_str(y, "name", ""));
}

/* Adds kind's collections holding games to *all. 0, or -1 with err set. */
static int collections_of(collection_kind kind, const char *me, cJSON ***all, int *n, int *cap, int *refused, char *err,
                          int en) {
    long st = 0;
    cJSON *list = romm_get_lean(g_collection_kinds[kind].list_path, COLLECTION_DROP, &st);
    if (st == 404 && kind != COLLECTION_REGULAR) { /* a RomM from before that kind */
        cJSON_Delete(list);
        return 0;
    }
    if (st != 200 || !cJSON_IsArray(list)) {
        if (st == 403) {
            snprintf(err, (size_t)en, "RomM didn't allow reading collections: pair RommPS again to grant it");
            *refused = 1;
        }
        else if (st || !romm_no_answer(err, en))
            snprintf(err, (size_t)en, "could not load collections (HTTP %ld)", st);
        cJSON_Delete(list);
        return -1;
    }
    const cJSON *c;
    cJSON_ArrayForEach(c, list) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(c, "id");
        int count = (int)jget_num(c, "rom_count", 0);
        if (count <= 0) continue;
        char id_text[512]; /* a virtual one's: base64 of its name and type */
        if (cJSON_IsString(id)) str_copy(id_text, sizeof id_text, id->valuestring);
        else if (cJSON_IsNumber(id)) snprintf(id_text, sizeof id_text, "%d", id->valueint);
        else continue;
        if (*n == *cap) {
            int grown_cap = *cap ? *cap * 2 : 16;
            cJSON **grown = realloc(*all, sizeof **all * (size_t)grown_cap);
            if (!grown) break;
            *all = grown;
            *cap = grown_cap;
        }
        /* Its own cover, else the first of its games' RomM puts together. */
        const char *cover = jget_str(c, "path_cover_small", "");
        const cJSON *covers = cJSON_GetObjectItemCaseSensitive(c, "path_covers_small");
        if (!cover[0] && cJSON_IsString(cJSON_GetArrayItem(covers, 0)))
            cover = cJSON_GetArrayItem(covers, 0)->valuestring;
        /* Another user's (RomM lists their public ones too). Virtual
         * collections are nobody's, and with no user known all are ours. */
        const char *owner = jget_str(c, "owner_username", "");
        if (!me[0] || !strcmp(owner, me)) owner = "";
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "kind", g_collection_kinds[kind].name);
        /* What a virtual one gathers by: "genre", "franchise", "company"... */
        if (kind == COLLECTION_VIRTUAL) cJSON_AddStringToObject(o, "type", jget_str(c, "type", ""));
        cJSON_AddStringToObject(o, "id", id_text);
        cJSON_AddStringToObject(o, "name", jget_str(c, "name", ""));
        /* A smart collection with no description says what it gathers. */
        const char *about = jget_str(c, "description", "");
        cJSON_AddStringToObject(o, "description", about[0] ? about : jget_str(c, "filter_summary", ""));
        cJSON_AddNumberToObject(o, "rom_count", count);
        cJSON_AddStringToObject(o, "cover", cover);
        cJSON_AddStringToObject(o, "owner", owner);
        (*all)[(*n)++] = o;
    }
    cJSON_Delete(list);
    return 0;
}

cJSON *library_collections(collection_kind only, int *refused, char *err, int en) {
    *refused = 0;
    if (only != COLLECTION_NONE && !collection_offered(only)) {
        snprintf(err, (size_t)en, "unknown kind of collection");
        return NULL;
    }
    char me[sizeof g_cfg.username];
    config_lock();
    str_copy(me, sizeof me, g_cfg.username);
    config_unlock();
    cJSON **all = NULL;
    int n = 0, cap = 0, failed = 0;
    for (int k = 0; k < COLLECTION_KINDS && !failed; k++)
        if (collection_offered((collection_kind)k) && (only == COLLECTION_NONE || only == (collection_kind)k))
            failed = collections_of((collection_kind)k, me, &all, &n, &cap, refused, err, en) < 0;
    cJSON *out = failed ? NULL : cJSON_CreateArray();
    if (out && n) qsort(all, (size_t)n, sizeof *all, collection_order);
    for (int i = 0; i < n; i++) {
        if (out) cJSON_AddItemToArray(out, all[i]);
        else cJSON_Delete(all[i]);
    }
    free(all);
    return out;
}

/* An extra emulator turned on for a system gets the games already there, not
 * only the ones downloaded from now on: each game in the system's main
 * emulator's folder is linked into the extra's (a copy across drives). */
/* A game folder a download is still filling (its files arrive as .part). */
static int still_downloading(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int part = 0;
    while (d && !part && (de = readdir(d))) part = ext_in_list(de->d_name, ".part");
    if (d) closedir(d);
    return part;
}

/* The copy in an extra emulator's folder is recorded like the game it came
 * from, so the game's saves sync there too and removing the game removes it. */
static void record_link(const char *from, const char *to, const profile_map *x) {
    state_lock();
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(state_section("roms"), from);
    if (src && jget_num(src, "rom_id", 0) > 0) {
        cJSON *e = state_entry_ensure("roms", to);
        const cJSON *f;
        cJSON_ArrayForEach(f, src) if (!cJSON_GetObjectItemCaseSensitive(e, f->string))
            cJSON_AddItemToObject(e, f->string, cJSON_Duplicate(f, 1));
        jset_str(e, "profile", x->profile_id);
        jset_str(e, "platform_dir", x->platform_dir);
        state_save();
    }
    state_unlock();
}

/* The extras that play the main emulator's games through a mount (see
 * profile_map.mount_games), for the console to put on when they start. */
static void update_mounts(const profile_map *maps, int n) {
    plat_mount *want = calloc((size_t)(n > 0 ? 2 * n : 1), sizeof *want);
    int nw = 0;
    for (int i = 0; want && i < n; i++) {
        const profile_map *x = &maps[i];
        /* Its games, then its saves (Dolphin's folders). */
        const char *title[2] = {x->mount_title, x->saves_title}, *from[2] = {x->rom_dir, x->saves_from},
                   *into[2] = {x->mount_into, x->saves_into}, *dir[2] = {x->own_rom_dir, x->saves_dir};
        for (int s = 0; s < 2 && nw < 2 * n; s++) {
            if (!title[s][0]) continue;
            int dup = 0;
            for (int k = 0; k < nw && !dup; k++) dup = !strcmp(want[k].title, title[s]) && !strcmp(want[k].into, into[s]);
            if (dup) continue;
            mkdir_p(dir[s]); /* to mount on */
            str_copy(want[nw].title, sizeof want[nw].title, title[s]);
            str_copy(want[nw].name, sizeof want[nw].name, x->profile_name);
            str_copy(want[nw].from, sizeof want[nw].from, from[s]);
            str_copy(want[nw].into, sizeof want[nw].into, into[s]);
            nw++;
        }
    }
    plat_mounts_set(want, nw);
    free(want);
}

/* An extra that could play the main emulator's games through a mount but
 * has games of its own in its folder (it was the main one before): the ones
 * RommPS downloaded move to the main one's folder, a rename on the same
 * drive, so the mount can go on. Files it didn't download stay, and so does
 * the copy. Returns how many moved. */
static int move_to_main(const profile_map *maps, int n) {
    int moved = 0;
    for (int i = 0; i < n; i++) {
        const profile_map *x = &maps[i];
        if (x->twin || !x->mount_blocked || plat_is_mount(x->rom_dir)) continue;
        char sys[64];
        str_copy(sys, sizeof sys, x->romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        const profile_map *m = profile_find_for_platform(maps, n, sys, NULL);
        if (!m || m == x || m->extra) continue;
        DIR *d = opendir(x->rom_dir);
        struct dirent *de;
        int here = 0;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.' || ext_in_list(de->d_name, ".part,.tmp")) continue;
            char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
            path_join(from, sizeof from, x->rom_dir, de->d_name);
            path_join(to, sizeof to, m->rom_dir, de->d_name);
            if (file_exists(to) || dir_exists(to)) continue; /* a copy: cleared once the mount is on */
            state_lock();
            const cJSON *rec = cJSON_GetObjectItemCaseSensitive(state_section("roms"), from);
            const int ours = rec && jget_num(rec, "rom_id", 0) > 0;
            state_unlock();
            if (!ours || mkdir_p(m->rom_dir) != 0 || rename(from, to) != 0) continue;
            state_lock();
            cJSON *e = cJSON_DetachItemFromObjectCaseSensitive(state_section("roms"), from);
            const int rom_id = e ? (int)jget_num(e, "rom_id", 0) : 0;
            if (e) {
                jset_str(e, "profile", m->profile_id);
                jset_str(e, "platform_dir", m->platform_dir);
                cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), to);
                cJSON_AddItemToObject(state_section("roms"), to, e);
            }
            state_save();
            state_unlock();
            LOGI("moved %s to %s: %s plays it from there now", from, to, m->profile_name);
            char why[160];
            snprintf(why, sizeof why, "From %s's folder to %s's", x->profile_name, m->profile_name);
            events_add("game", "moved", rom_id, to, x->profile_name, why);
            here++;
        }
        if (d) closedir(d);
        if (here) {
            events_save();
            plat_notify("RommPS: %d %s game%s moved to %s", here, x->profile_name, here == 1 ? "" : "s", m->profile_name);
        }
        moved += here;
    }
    return moved;
}

int library_own_games(const char *root, const char *system, const char *action, char *err, int en) {
    const int move = !strcmp(action, "move");
    if (!move && strcmp(action, "delete") != 0) {
        snprintf(err, (size_t)en, "action is move or delete");
        return -1;
    }
    profile_map *maps = NULL;
    int n = load_maps(&maps), done = 0;
    const profile_map *x = NULL, *m = NULL;
    for (int i = 0; i < n && !x; i++) {
        char sys[64];
        str_copy(sys, sizeof sys, maps[i].romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        if (maps[i].extra && maps[i].mount_blocked && !strcmp(maps[i].root, root) && !strcmp(sys, system)) x = &maps[i];
    }
    if (x) {
        char sys[64];
        str_copy(sys, sizeof sys, x->romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        m = profile_find_for_platform(maps, n, sys, NULL);
    }
    if (!x || !m || m == x || m->extra) {
        snprintf(err, (size_t)en, "no extra emulator at %s for %s with games in the way of its mount", root, system);
        profiles_free(maps);
        return -1;
    }
    if (plat_is_mount(x->rom_dir)) {
        snprintf(err, (size_t)en, "close %s first", x->profile_name);
        profiles_free(maps);
        return -1;
    }
    DIR *d = opendir(x->rom_dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.' || ext_in_list(de->d_name, ".part,.tmp") || library_not_game(de->d_name)) continue;
        char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
        path_join(from, sizeof from, x->rom_dir, de->d_name);
        path_join(to, sizeof to, m->rom_dir, de->d_name);
        if (file_exists(to) || dir_exists(to)) continue; /* a copy */
        if (move && (mkdir_p(m->rom_dir) != 0 || rename(from, to) != 0)) {
            LOGW("could not move %s to %s", from, to);
            continue;
        }
        if (!move) remove_tree(from);
        state_lock();
        cJSON *e = cJSON_DetachItemFromObjectCaseSensitive(state_section("roms"), from);
        const int rom_id = e ? (int)jget_num(e, "rom_id", 0) : 0;
        if (e && move) {
            jset_str(e, "profile", m->profile_id);
            jset_str(e, "platform_dir", m->platform_dir);
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), to);
            cJSON_AddItemToObject(state_section("roms"), to, e);
        } else {
            cJSON_Delete(e);
        }
        state_save();
        state_unlock();
        char why[160];
        snprintf(why, sizeof why, move ? "From %s's folder to %s's" : "Deleted from %s's folder", x->profile_name,
                 m->profile_name);
        events_add("game", move ? "moved" : "removed", rom_id, move ? to : from, x->profile_name, why);
        LOGI("%s %s%s%s", move ? "moved" : "deleted", from, move ? " to " : "", move ? to : "");
        done++;
    }
    if (d) closedir(d);
    events_save();
    profiles_free(maps);
    library_link_extras(); /* the mount can go on now */
    return done;
}

/* Whether b holds the same game as a: the same size, file by file. */
static int same_game(const char *a, const char *b) {
    if (!dir_exists(a)) return file_exists(b) && file_size(a) == file_size(b);
    if (!dir_exists(b)) return 0;
    DIR *d = opendir(a);
    struct dirent *de;
    int same = d != NULL;
    while (same && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char x[PATH_MAX_LEN], y[PATH_MAX_LEN];
        path_join(x, sizeof x, a, de->d_name);
        path_join(y, sizeof y, b, de->d_name);
        same = same_game(x, y);
    }
    if (d) closedir(d);
    return same;
}

static int64_t game_size(const char *path) {
    if (!dir_exists(path)) return file_size(path) > 0 ? file_size(path) : 0;
    DIR *d = opendir(path);
    struct dirent *de;
    int64_t total = 0;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[PATH_MAX_LEN];
        path_join(sub, sizeof sub, path, de->d_name);
        total += game_size(sub);
    }
    if (d) closedir(d);
    return total;
}

/* Copies made for an extra before it had a mount: the main emulator has the
 * same game, so they go, and so do their records. */
static void clear_old_copies(const profile_map *maps, int n) {
    for (int i = 0; i < n; i++) {
        const profile_map *x = &maps[i];
        if (!x->mount_title[0]) continue;
        int dup = 0;
        for (int k = 0; k < i && !dup; k++) dup = !strcmp(maps[k].own_rom_dir, x->own_rom_dir);
        /* Our mount on it while the app runs: those are the main one's
         * games. A folder another emulator plays from keeps its games. */
        for (int k = 0; k < n && !dup; k++) dup = !strcmp(maps[k].rom_dir, x->own_rom_dir);
        if (dup || plat_is_mount(x->own_rom_dir)) continue;
        DIR *d = opendir(x->own_rom_dir);
        struct dirent *de;
        int gone = 0;
        int64_t freed = 0;
        while (d && (de = readdir(d))) {
            /* Not a game (Crux2's README): the emulator's own, never a copy. */
            if (de->d_name[0] == '.' || library_not_game(de->d_name)) continue;
            char copy[PATH_MAX_LEN], game[PATH_MAX_LEN];
            path_join(copy, sizeof copy, x->own_rom_dir, de->d_name);
            path_join(game, sizeof game, x->rom_dir, de->d_name);
            /* The app started meanwhile: its mount shows the main one's games
             * under the copy's name, and removing "the copy" would remove them. */
            struct stat cs, gs;
            if (plat_is_mount(x->own_rom_dir) ||
                (stat(copy, &cs) == 0 && stat(game, &gs) == 0 && cs.st_ino == gs.st_ino))
                break;
            if (!same_game(copy, game)) {
                LOGW("%s: %s isn't the same as %s, kept", x->profile_name, copy, game);
                continue;
            }
            const int64_t size = game_size(copy);
            remove_tree(copy);
            if (file_exists(copy) || dir_exists(copy)) {
                LOGW("%s: could not remove the copy %s", x->profile_name, copy);
                continue;
            }
            state_lock();
            const cJSON *rec = cJSON_GetObjectItemCaseSensitive(state_section("roms"), copy);
            const int rom_id = rec ? (int)jget_num(rec, "rom_id", 0) : 0;
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), copy);
            state_save();
            state_unlock();
            events_add("game", "removed", rom_id, copy, x->profile_name, "A copy: it plays the main emulator's now");
            gone++;
            freed += size;
        }
        if (d) closedir(d);
        if (gone) {
            LOGI("%s plays %s's games now: removed %d copies (%lld bytes)", x->profile_name, x->rom_dir, gone, (long long)freed);
            events_save();
            plat_notify("RommPS: %s now plays the same game files\n%.1f GB freed", x->profile_name, (double)freed / 1e9);
        }
    }
}

/* Switch games downloaded before 1.3 as a folder each, which the emulator
 * doesn't look in: the files move out, the game's to rom_dir and its update's
 * and DLC's to theirs, each recorded as the game. An empty folder (a download
 * that never finished) goes; one holding an archive stays as it is. */
static void nx_unfold(const profile_map *maps, int n) {
    typedef struct { char path[PATH_MAX_LEN]; int map; } folder;
    folder *todo = NULL;
    int nt = 0, cap = 0, logged = 0;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        if (jget_num(e, "rom_id", 0) <= 0) continue;
        char parent[PATH_MAX_LEN];
        str_copy(parent, sizeof parent, e->string);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = 0;
        for (int i = 0; i < n; i++) {
            const profile_map *m = &maps[i];
            if (strncmp(m->romm_slugs, "switch", 6) != 0 || (m->romm_slugs[6] && m->romm_slugs[6] != ',') ||
                strcmp(jget_str(e, "profile", ""), m->profile_id) != 0 || strcmp(parent, m->rom_dir) != 0)
                continue;
            if (nt == cap) {
                cap = cap ? cap * 2 : 8;
                folder *grown = realloc(todo, sizeof *todo * (size_t)cap);
                if (!grown) break;
                todo = grown;
            }
            str_copy(todo[nt].path, sizeof todo[nt].path, e->string);
            todo[nt++].map = i;
            break;
        }
    }
    state_unlock();
    for (int t = 0; t < nt; t++) {
        const char *dir = todo[t].path;
        const profile_map *m = &maps[todo[t].map];
        if (!dir_exists(dir) || still_downloading(dir)) continue;
        state_lock();
        cJSON *rec = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(state_section("roms"), dir), 1);
        state_unlock();
        if (!rec) continue;
        const int rom_id = (int)jget_num(rec, "rom_id", 0);
        int moved = 0, left = 0;
        DIR *d = opendir(dir);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
            path_join(from, sizeof from, dir, de->d_name);
            const nx_kind k = nx_file_kind(de->d_name, "");
            path_join(to, sizeof to, nx_dir(m, k), de->d_name);
            if (dir_exists(from) || ext_in_list(de->d_name, ".rar,.zip,.7z") || file_exists(to) ||
                mkdir_p(nx_dir(m, k)) != 0 || move_file(from, to) != 0) {
                left++;
                continue;
            }
            state_lock();
            cJSON *ne = cJSON_Duplicate(rec, 1);
            jset_str(ne, "fs_name", de->d_name);
            cJSON_DeleteItemFromObjectCaseSensitive(ne, "md5");
            cJSON_DeleteItemFromObjectCaseSensitive(ne, "stems");
            char stem[256];
            path_stem(stem, sizeof stem, de->d_name);
            cJSON_AddItemToArray(cJSON_AddArrayToObject(ne, "stems"), cJSON_CreateString(stem));
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), to);
            cJSON_AddItemToObject(state_section("roms"), to, ne);
            state_save();
            state_unlock();
            moved++;
        }
        if (d) closedir(d);
        if (left) {
            if (moved) LOGI("%s: %d file%s moved out, %d left (archives stay)", dir, moved, moved == 1 ? "" : "s", left);
        } else {
            rmdir(dir);
            state_lock();
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), dir);
            state_save();
            state_unlock();
            LOGI("%s: %s", dir, moved ? "unfolded, the emulator sees it now" : "empty, removed");
            events_add("game", moved ? "moved" : "removed", rom_id, dir, m->profile_name,
                       moved ? "Out of its folder, where the emulator sees it" : "An empty folder: the download never finished");
            logged++;
        }
        cJSON_Delete(rec);
        if (moved || !left) installed_changed();
    }
    if (logged) events_save();
    free(todo);
}

static pthread_mutex_t g_link_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_link_running, g_link_again;

static void *link_extras_thread(void *arg) {
    (void)arg;
again:;
    profile_map *maps = NULL;
    int n = load_maps(&maps), linked = 0;
    if (move_to_main(maps, n)) { /* now it can have a mount */
        profiles_free(maps);
        maps = NULL;
        n = load_maps(&maps);
    }
    clear_old_copies(maps, n); /* before the mounts can hide them */
    update_mounts(maps, n);
    nx_unfold(maps, n);
    sync_nx_settle(maps, n);
    for (int i = 0; i < n; i++) {
        const profile_map *x = &maps[i];
        if (!x->extra || x->twin) continue;
        char first[64];
        str_copy(first, sizeof first, x->romm_slugs);
        char *comma = strchr(first, ',');
        if (comma) *comma = 0;
        const profile_map *m = profile_find_for_platform(maps, n, first, NULL);
        if (!m || m == x || m->extra || !strcmp(m->rom_dir, x->rom_dir)) continue;
        /* A folder an extra's mount covers gets no copies. */
        int covered = plat_is_mount(x->rom_dir);
        for (int k = 0; k < n && !covered; k++) covered = maps[k].mount_title[0] && !strcmp(maps[k].own_rom_dir, x->rom_dir);
        if (covered) continue;
        /* A folder several of the main emulator's systems share (Porpoise's
         * games: GameCube and Wii): only the games matched for this system. */
        int shared = 0;
        for (int k = 0; k < n; k++) shared |= &maps[k] != m && !maps[k].twin && !strcmp(maps[k].rom_dir, m->rom_dir);
        DIR *d = opendir(m->rom_dir);
        struct dirent *de;
        int here = 0;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.' || ext_in_list(de->d_name, m->save_exts) || ext_in_list(de->d_name, m->state_exts) ||
                ext_in_list(de->d_name, ".part,.tmp"))
                continue;
            char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
            path_join(from, sizeof from, m->rom_dir, de->d_name);
            if (shared) {
                state_lock();
                const cJSON *rec = cJSON_GetObjectItemCaseSensitive(state_section("roms"), from);
                const int ours = rec && jget_num(rec, "rom_id", 0) > 0 && !strcmp(jget_str(rec, "platform_dir", ""), m->platform_dir);
                state_unlock();
                if (!ours) continue;
            }
            path_join(to, sizeof to, x->rom_dir, de->d_name);
            if (file_exists(to) || dir_exists(to) || (dir_exists(from) && still_downloading(from))) continue;
            mkdir_p(x->rom_dir);
            if (link_or_copy(from, to) == 0) {
                record_link(from, to, x);
                here++;
            } else {
                LOGW("could not put %s in %s", de->d_name, x->rom_dir);
            }
        }
        if (d) closedir(d);
        if (here) {
            char sys[64];
            str_copy(sys, sizeof sys, x->platform_dir);
            for (char *c = sys; *c; c++)
                if (*c >= 'a' && *c <= 'z') *c -= 32;
            plat_notify("RommPS: %d %s %s with %s", here, sys, here == 1 ? "game now also syncs" : "games now also sync",
                        x->profile_name);
            linked += here;
        }
    }
    if (config_is_paired()) firmware_sync(maps, n, NULL, 0, NULL); /* emulators just turned on */
    profiles_free(maps);
    if (linked) {
        LOGI("extra emulators: %d games linked in", linked);
        sync_request("extra emulators");
    }
    /* One at a time; settings saved meanwhile get one more round. */
    pthread_mutex_lock(&g_link_lock);
    int again = g_link_again;
    g_link_again = 0;
    g_link_running = again;
    pthread_mutex_unlock(&g_link_lock);
    if (again) goto again;
    return NULL;
}

void library_link_extras(void) {
    pthread_mutex_lock(&g_link_lock);
    int start = !g_link_running;
    if (start) g_link_running = 1;
    else g_link_again = 1;
    pthread_mutex_unlock(&g_link_lock);
    if (start && thread_start(link_extras_thread, NULL) != 0) {
        pthread_mutex_lock(&g_link_lock);
        g_link_running = 0;
        pthread_mutex_unlock(&g_link_lock);
    }
}

/* What RommPS shows about a game: RomM's metadata (release year, publishers,
 * genres, players, rating, summary, How Long to Beat), the user's own data
 * (last played, status, completion, rating) and the play time RomM has
 * recorded. Kept a few minutes, so moving through a shelf stays quick. */
#define DETAILS_SLOTS 24
#define DETAILS_SEC   300
static struct {
    int id;
    double at;
    cJSON *details;
} g_details[DETAILS_SLOTS];
static pthread_mutex_t g_details_lock = PTHREAD_MUTEX_INITIALIZER;

static void join_strings(cJSON *out, const char *key, const cJSON *arr, int max) {
    char buf[256] = "";
    int n = 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, arr) {
        if (!cJSON_IsString(x) || !x->valuestring[0] || n == max) continue;
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "%s%s", n ? ", " : "", x->valuestring);
        n++;
    }
    if (buf[0]) cJSON_AddStringToObject(out, key, buf);
}

cJSON *library_details(int rom_id, char *err, int en) {
    double now = mono_now();
    pthread_mutex_lock(&g_details_lock);
    for (int i = 0; i < DETAILS_SLOTS; i++)
        if (g_details[i].details && g_details[i].id == rom_id && now - g_details[i].at < DETAILS_SEC) {
            cJSON *hit = cJSON_Duplicate(g_details[i].details, 1);
            pthread_mutex_unlock(&g_details_lock);
            return hit;
        }
    pthread_mutex_unlock(&g_details_lock);

    char path[160];
    snprintf(path, sizeof path, "/api/roms/%d", rom_id);
    long st = 0;
    cJSON *rom = romm_call("GET", path, NULL, &st);
    if (st != 200 || !rom) {
        if (st || !romm_no_answer(err, en)) snprintf(err, (size_t)en, "could not load the game from RomM (HTTP %ld)", st);
        cJSON_Delete(rom);
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", rom_id);
    add_version(out, rom);
    /* What a download would bring, or why it can't. */
    const int nx = is_switch(jget_str(rom, "platform_slug", ""));
    if (!nx) add_version_list(out, rom);
    const char *archive = rom_archive(rom, nx);
    if (archive) {
        char msg[200];
        snprintf(msg, sizeof msg, ARCHIVED_MSG, archive);
        cJSON_AddStringToObject(out, "archived", msg);
    }
    unsigned long long game;
    /* A Switch game's title id, and its update and DLC, in its folder or as
     * games of their own: [{"name", "kind", "size", "title_id"}], updates
     * first. A folder holding a bundle (a collection's games, each with its
     * own title id) lists the other games and theirs apart, as "bundle":
     * they come with the download, but aren't this game's. */
    if (nx && !archive && nx_rom(rom, &game) == NX_GAME) {
        char tid[17];
        snprintf(tid, sizeof tid, "%016llX", game);
        cJSON_AddStringToObject(out, "title_id", tid);
        cJSON *list = cJSON_CreateArray(), *found = nx_addons((int)jget_num(rom, "platform_id", 0), game), *a;
        cJSON *bundle = cJSON_CreateArray();
        const cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files")) {
            if (extra_file(f)) continue;
            const char *name = jget_str(f, "file_name", "");
            unsigned long long of = 0;
            const nx_kind titled = nx_title(name, &of, NULL);
            const nx_kind k = nx_file_kind(name, jget_str(f, "category", ""));
            const int other = titled != NX_NONE && of != game;
            if (!other && k != NX_UPDATE && k != NX_DLC) continue;
            a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "kind", k == NX_UPDATE ? "update" : k == NX_DLC ? "dlc" : "game");
            cJSON_AddStringToObject(a, "name", name);
            cJSON_AddStringToObject(a, "title_id", nx_title_hex(name, tid));
            cJSON_AddNumberToObject(a, "size", jget_num(f, "file_size_bytes", 0));
            cJSON_AddItemToArray(other ? bundle : found, a);
        }
        if (cJSON_GetArraySize(bundle)) cJSON_AddItemToObject(out, "bundle", bundle);
        else cJSON_Delete(bundle);
        int updates = 0, dlc = 0;
        for (int pass = 0; pass < 2; pass++)
            cJSON_ArrayForEach(a, found) {
                const int up = !strcmp(jget_str(a, "kind", ""), "update");
                if (up != !pass) continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "kind", up ? "update" : "dlc");
                cJSON_AddStringToObject(o, "name", jget_str(a, "name", ""));
                cJSON_AddStringToObject(o, "title_id", jget_str(a, "title_id", ""));
                cJSON_AddNumberToObject(o, "size", jget_num(a, "size", 0));
                cJSON_AddItemToArray(list, o);
                updates += up;
                dlc += !up;
            }
        cJSON_Delete(found);
        if (updates) cJSON_AddNumberToObject(out, "updates", updates);
        if (dlc) cJSON_AddNumberToObject(out, "dlc", dlc);
        if (cJSON_GetArraySize(list)) cJSON_AddItemToObject(out, "addons", list);
        else cJSON_Delete(list);
    }
    const char *summary = jget_str(rom, "summary", "");
    if (summary[0]) cJSON_AddStringToObject(out, "summary", summary);
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(rom, "metadatum");
    /* A release date in seconds or milliseconds since 1970, as a year. */
    double date = jget_num(meta, "first_release_date", 0);
    if (date > 1e11) date /= 1000.0;
    int year = 0;
    if (date > 0) {
        time_t t = (time_t)date;
        struct tm tm;
        gmtime_r(&t, &tm);
        year = tm.tm_year + 1900;
    }
    const cJSON *hltb = cJSON_GetObjectItemCaseSensitive(rom, "hltb_metadata");
    if (!year) year = (int)jget_num(hltb, "release_year", 0);
    if (year) cJSON_AddNumberToObject(out, "year", year);
    join_strings(out, "publishers", cJSON_GetObjectItemCaseSensitive(meta, "publishers"), 2);
    join_strings(out, "developers", cJSON_GetObjectItemCaseSensitive(meta, "developers"), 2);
    join_strings(out, "genres", cJSON_GetObjectItemCaseSensitive(meta, "genres"), 3);
    const char *players = jget_str(meta, "player_count", "");
    if (players[0]) cJSON_AddStringToObject(out, "players", players);
    double rating = jget_num(meta, "average_rating", 0);
    if (rating > 0) cJSON_AddNumberToObject(out, "rating", rating);
    double main_story = jget_num(hltb, "main_story", 0);
    if (main_story > 0) cJSON_AddNumberToObject(out, "main_story_s", main_story);
    const cJSON *mine = cJSON_GetObjectItemCaseSensitive(rom, "rom_user");
    const char *last = jget_str(mine, "last_played", "");
    if (last[0]) cJSON_AddStringToObject(out, "last_played", last);
    const char *status = jget_str(mine, "status", "");
    if (status[0]) cJSON_AddStringToObject(out, "status", status);
    if (jget_num(mine, "completion", 0) > 0) cJSON_AddNumberToObject(out, "completion", jget_num(mine, "completion", 0));
    if (jget_num(mine, "rating", 0) > 0) cJSON_AddNumberToObject(out, "my_rating", jget_num(mine, "rating", 0));
    cJSON_Delete(rom);

    /* Play time: the sessions RomM has for the game, from every device. */
    double played_ms = 0;
    int sessions = 0;
    for (int offset = 0; offset < 2000; offset += 200) {
        char sp[160];
        snprintf(sp, sizeof sp, "/api/play-sessions?rom_id=%d&limit=200&offset=%d", rom_id, offset);
        cJSON *list = romm_call("GET", sp, NULL, &st);
        const cJSON *items = cJSON_IsArray(list) ? list : cJSON_GetObjectItemCaseSensitive(list, "items"), *x;
        int got = 0;
        cJSON_ArrayForEach(x, items) {
            played_ms += jget_num(x, "duration_ms", 0);
            got++;
        }
        sessions += got;
        cJSON_Delete(list);
        if (st != 200 || got < 200) break;
    }
    if (sessions) {
        cJSON_AddNumberToObject(out, "play_ms", played_ms);
        cJSON_AddNumberToObject(out, "sessions", sessions);
    }

    pthread_mutex_lock(&g_details_lock);
    int slot = 0;
    for (int i = 0; i < DETAILS_SLOTS; i++) {
        if (!g_details[i].details || g_details[i].id == rom_id) {
            slot = i;
            break;
        }
        if (g_details[i].at < g_details[slot].at) slot = i;
    }
    cJSON_Delete(g_details[slot].details);
    g_details[slot].id = rom_id;
    g_details[slot].at = now;
    g_details[slot].details = cJSON_Duplicate(out, 1);
    pthread_mutex_unlock(&g_details_lock);
    return out;
}

/* The games on this console, most recently played first: when one of its saves
 * last changed, or else when its file did. [{id, name, fs_name, platform_id,
 * cover, profile, played}], at most `limit`. */
/* Games recorded before the state kept a game's cover and platform get them
 * from RomM, a few per call so a first look stays quick. */
#define INSTALLED_BACKFILL 6
static void backfill_installed(void) {
    int ids[INSTALLED_BACKFILL], n = 0;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        int id = (int)jget_num(e, "rom_id", 0);
        if (id <= 0 || cJSON_GetObjectItemCaseSensitive(e, "platform_id") || n == INSTALLED_BACKFILL) continue;
        int seen = 0;
        for (int i = 0; i < n; i++) seen |= ids[i] == id;
        if (!seen) ids[n++] = id;
    }
    state_unlock();
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, "/api/roms/%d", ids[i]);
        long st = 0;
        cJSON *rom = romm_call("GET", path, NULL, &st);
        if (!rom) continue;
        state_lock();
        cJSON *x;
        cJSON_ArrayForEach(x, state_section("roms")) {
            if ((int)jget_num(x, "rom_id", 0) != ids[i]) continue;
            jset_num(x, "platform_id", jget_num(rom, "platform_id", 0));
            jset_str(x, "cover", jget_str(rom, "path_cover_small", ""));
        }
        state_save();
        state_unlock();
        cJSON_Delete(rom);
    }
}

/* A game's size here: its file, or everything in its folder. */
static double local_size(const char *path, int depth) {
    if (!dir_exists(path)) return (double)(file_size(path) > 0 ? file_size(path) : 0);
    double total = 0;
    DIR *d = depth < 3 ? opendir(path) : NULL;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX_LEN];
        path_join(p, sizeof p, path, de->d_name);
        total += local_size(p, depth + 1);
    }
    if (d) closedir(d);
    return total;
}

static int newest_first(const void *a, const void *b) {
    double x = jget_num(*(cJSON *const *)a, "played", 0), y = jget_num(*(cJSON *const *)b, "played", 0);
    return x < y ? 1 : x > y ? -1 : 0;
}

cJSON *library_installed(int limit) {
    backfill_installed();
    cJSON *out = cJSON_CreateArray();
    const cJSON *e, *sv;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        int id = (int)jget_num(e, "rom_id", 0);
        if (id <= 0 || jget_num(e, "with", 0) > 0 || !(file_exists(e->string) || dir_exists(e->string))) continue;
        double played = (double)file_mtime(e->string);
        cJSON_ArrayForEach(sv, state_section("saves"))
            if ((int)jget_num(sv, "rom_id", 0) == id && jget_num(sv, "mtime", 0) > played) played = jget_num(sv, "mtime", 0);
        cJSON *dup = NULL, *o;
        cJSON_ArrayForEach(o, out) if ((int)jget_num(o, "id", 0) == id) dup = o;
        if (dup) {
            if (played > jget_num(dup, "played", 0)) jset_num(dup, "played", played);
            continue;
        }
        o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", id);
        cJSON_AddStringToObject(o, "name", jget_str(e, "name", ""));
        cJSON_AddStringToObject(o, "fs_name", jget_str(e, "fs_name", ""));
        cJSON_AddNumberToObject(o, "platform_id", jget_num(e, "platform_id", 0));
        cJSON_AddStringToObject(o, "cover", jget_str(e, "cover", ""));
        cJSON_AddStringToObject(o, "profile", jget_str(e, "profile", ""));
        cJSON_AddNumberToObject(o, "played", played);
        cJSON_AddStringToObject(o, "path", e->string); /* sized below, off the lock */
        cJSON_AddItemToArray(out, o);
    }
    state_unlock();
    /* Newest first. */
    int n = cJSON_GetArraySize(out), k = 0;
    cJSON **all = malloc(sizeof *all * (size_t)(n ? n : 1)), *o;
    cJSON *sorted = cJSON_CreateArray();
    if (!all) {
        cJSON_Delete(out);
        return sorted;
    }
    while ((o = cJSON_DetachItemFromArray(out, 0))) all[k++] = o;
    cJSON_Delete(out);
    qsort(all, (size_t)n, sizeof *all, newest_first);
    for (k = 0; k < n; k++) {
        if (k < limit) {
            cJSON_AddNumberToObject(all[k], "size", local_size(jget_str(all[k], "path", ""), 0));
            cJSON_DeleteItemFromObjectCaseSensitive(all[k], "path");
            cJSON_AddItemToArray(sorted, all[k]);
        } else {
            cJSON_Delete(all[k]);
        }
    }
    free(all);
    return sorted;
}

/* Whether a game is downloaded: a local file or folder the state maps to it.
 * Looked at after the state lock, like the fingerprint below. */
#define ROM_PLACES 8 /* one per emulator that has it, a few at most */
int library_on_console(int id) {
    char paths[ROM_PLACES][PATH_MAX_LEN];
    int n = 0;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms"))
        if ((int)jget_num(e, "rom_id", -1) == id && n < ROM_PLACES) str_copy(paths[n++], PATH_MAX_LEN, e->string);
    state_unlock();
    for (int i = 0; i < n; i++)
        if (file_exists(paths[i]) || dir_exists(paths[i])) return 1;
    return 0;
}

cJSON *library_discs_here(int id) {
    cJSON *here = NULL;
    const cJSON *e, *disc, *file;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        const cJSON *discs = cJSON_GetObjectItemCaseSensitive(e, "discs");
        if ((int)jget_num(e, "rom_id", -1) != id || !cJSON_IsObject(discs) || !dir_exists(e->string)) continue;
        here = cJSON_CreateArray();
        cJSON_ArrayForEach(disc, discs) {
            int all = 1;
            cJSON_ArrayForEach(file, disc) {
                char path[PATH_MAX_LEN];
                path_join(path, sizeof path, e->string, cJSON_IsString(file) ? file->valuestring : "");
                all = all && file_exists(path);
            }
            if (all) cJSON_AddItemToArray(here, cJSON_CreateNumber(atoi(disc->string)));
        }
        break;
    }
    state_unlock();
    return here;
}

/* The installed fingerprint, worked out lazily and kept: the status that
 * carries it is asked for every few seconds, and that never touches the
 * disk. It's looked at again at most every INSTALLED_SIG_SEC, never while a
 * game is running, and at once after our own downloads and deletes. The
 * paths are copied out under the state lock and looked at after it, so a
 * slow or sleeping drive never holds anyone else up. */
#define INSTALLED_SIG_SEC 15
static pthread_mutex_t g_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_sig;
static double g_sig_at;

unsigned library_installed_sig(void) {
    /* Someone else is working it out: theirs will do, this one says the last. */
    if (pthread_mutex_trylock(&g_sig_lock) != 0) return g_sig;
    const double now = mono_now();
    const char *env = getenv("ROMM_SYNC_INSTALLED_SIG_SEC"); /* shorter in tests */
    const double every = env && atof(env) > 0 ? atof(env) : INSTALLED_SIG_SEC;
    if (!g_sig_stale && (now - g_sig_at < every || sync_game_running())) {
        pthread_mutex_unlock(&g_sig_lock);
        return g_sig;
    }
    g_sig_stale = 0;
    int n = 0, cap = 0;
    struct { int id; char *path; } *all = NULL;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        int id = (int)jget_num(e, "rom_id", 0);
        if (id <= 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            void *grown = realloc(all, sizeof *all * (size_t)cap);
            if (!grown) break;
            all = grown;
        }
        all[n].id = id;
        all[n].path = strdup(e->string);
        if (all[n].path) n++;
    }
    state_unlock();
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) {
        if (file_exists(all[i].path) || dir_exists(all[i].path)) h = (h ^ (unsigned)all[i].id) * 16777619u;
        free(all[i].path);
    }
    free(all);
    g_sig = h;
    g_sig_at = now;
    pthread_mutex_unlock(&g_sig_lock);
    return h;
}

/* Recent library pages, so going back, changing letters or reopening the UI
 * doesn't fetch the same games again over a slow link. Installed flags are
 * worked out again on every hit. */
#define PAGE_CACHE_SLOTS 32
#define PAGE_CACHE_SEC   300
static struct {
    char key[3200]; /* the request path (up to 3072) and its offset and limit */
    double at;
    cJSON *page;
} g_pages[PAGE_CACHE_SLOTS];
static pthread_mutex_t g_pages_lock = PTHREAD_MUTEX_INITIALIZER;

static cJSON *page_cache_get(const char *key) {
    cJSON *hit = NULL;
    double now = mono_now();
    pthread_mutex_lock(&g_pages_lock);
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++)
        if (g_pages[i].page && !strcmp(g_pages[i].key, key) && now - g_pages[i].at < PAGE_CACHE_SEC)
            hit = cJSON_Duplicate(g_pages[i].page, 1);
    pthread_mutex_unlock(&g_pages_lock);
    return hit;
}

static void page_cache_put(const char *key, const cJSON *page) {
    int slot = 0;
    pthread_mutex_lock(&g_pages_lock);
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (!g_pages[i].page || !strcmp(g_pages[i].key, key)) {
            slot = i;
            break;
        }
        if (g_pages[i].at < g_pages[slot].at) slot = i;
    }
    cJSON_Delete(g_pages[slot].page);
    str_copy(g_pages[slot].key, sizeof g_pages[slot].key, key);
    g_pages[slot].at = mono_now();
    g_pages[slot].page = cJSON_Duplicate(page, 1);
    pthread_mutex_unlock(&g_pages_lock);
}

/* A game's card stands for every version of it: on the console when any is. */
static int any_on_console(const cJSON *o) {
    if (library_on_console((int)jget_num(o, "id", 0))) return 1;
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(o, "siblings"))
        if (library_on_console((int)s->valuedouble)) return 1;
    return 0;
}

/* What a Switch entry with no title id in its name turned out to be once
 * down (its record's "title_id"): an update or DLC, else NX_NONE. */
static nx_kind nx_found_as(int rom_id) {
    nx_kind k = NX_NONE;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        if ((int)jget_num(e, "rom_id", 0) != rom_id) continue;
        const switchid_kind x = switchid_kind_of(strtoull(jget_str(e, "title_id", "0"), NULL, 16));
        if (x == SWITCHID_UPDATE || x == SWITCHID_DLC) k = x == SWITCHID_UPDATE ? NX_UPDATE : NX_DLC;
    }
    state_unlock();
    return k;
}

/* Keeps only what the library page shows of each game. */
static int add_library_rom(const cJSON *it, const cJSON *page, int first, void *ctx) {
    cJSON *out = ctx;
    if (first && !cJSON_GetObjectItemCaseSensitive(out, "total")) {
        cJSON_AddNumberToObject(out, "total", jget_num(page, "total", 0));
        /* First letter -> offset of its first game, for the A-Z ribbon. */
        const cJSON *ci = cJSON_GetObjectItemCaseSensitive(page, "char_index");
        if (cJSON_IsObject(ci)) cJSON_AddItemToObject(out, "char_index", cJSON_Duplicate(ci, 1));
    }
    if (!it) return 0;
    int id = (int)jget_num(it, "id", 0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", jget_str(it, "name", jget_str(it, "fs_name", "")));
    cJSON_AddStringToObject(o, "fs_name", jget_str(it, "fs_name", ""));
    cJSON_AddNumberToObject(o, "size", jget_num(it, "fs_size_bytes", 0));
    cJSON_AddStringToObject(o, "cover", jget_str(it, "path_cover_small", ""));
    if (!library_not_game(jget_str(it, "fs_name", "")))
        covers_warm(jget_str(it, "path_cover_small", "")); /* fetched ahead, in the background */
    cJSON_AddNumberToObject(o, "platform_id", jget_num(it, "platform_id", 0));
    cJSON_AddBoolToObject(o, "multi", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "has_multiple_files")));
    /* A Switch game's update or DLC comes with the game, and isn't listed. */
    const int nx = is_switch(jget_str(it, "platform_slug", ""));
    unsigned long long game;
    nx_kind k = nx ? nx_title(jget_str(it, "fs_name", ""), &game, NULL) : NX_NONE;
    if (nx && k == NX_NONE) k = nx_found_as(id);
    if (library_not_game(jget_str(it, "fs_name", "")) || k == NX_UPDATE || k == NX_DLC) cJSON_AddBoolToObject(o, "not_game", 1);
    if (k == NX_UPDATE || k == NX_DLC) cJSON_AddStringToObject(o, "addon", k == NX_UPDATE ? "update" : "dlc");
    if (rom_archive(it, nx)) cJSON_AddBoolToObject(o, "archived", 1);
    if (nx) {
        /* What nx_collapse needs (and takes out again): the game's title id
         * and the entries RomM counts as versions of the same game. */
        char id[20];
        if (k == NX_GAME) {
            snprintf(id, sizeof id, "%016llX", game);
            cJSON_AddStringToObject(o, "_game", id);
        }
        cJSON *sib = cJSON_AddArrayToObject(o, "_sib");
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(it, "sibling_roms"))
            cJSON_AddItemToArray(sib, cJSON_CreateNumber(jget_num(s, "id", 0)));
    } else {
        add_version(o, it); /* a Switch game's "versions" are its files: one card */
    }
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(out, "items"), o);
    cJSON_AddBoolToObject(o, "installed", any_on_console(o));
    return 0;
}

/* A Switch game RomM keeps as several entries (the same game as an .nsp and an
 * .nsz, a .rar beside its files, two dumps) shows as one card: the one on the
 * console, else not an archive, else named by its title id, else the biggest.
 * The others are hidden like updates and DLC. Entries go together when RomM
 * counts them as versions of the same game, or they carry the same title id;
 * only within a page, where the A-Z order puts them side by side. */
static int nx_same(const cJSON *a, const cJSON *b) {
    const char *ga = jget_str(a, "_game", ""), *gb = jget_str(b, "_game", "");
    if (ga[0] && !strcmp(ga, gb)) return 1;
    const int ia = (int)jget_num(a, "id", 0), ib = (int)jget_num(b, "id", 0);
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(a, "_sib")) if ((int)s->valuedouble == ib) return 1;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(b, "_sib")) if ((int)s->valuedouble == ia) return 1;
    return 0;
}

static double nx_rank(const cJSON *o) {
    return (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "installed")) ? 8e15 : 0) +
           (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "archived")) ? 0 : 4e15) +
           (jget_str(o, "_game", "")[0] ? 2e15 : 0) + jget_num(o, "size", 0);
}

static void nx_collapse(cJSON *page) {
    cJSON *items = cJSON_GetObjectItemCaseSensitive(page, "items");
    const int n = cJSON_GetArraySize(items);
    int *group = malloc(sizeof *group * (size_t)(n ? n : 1));
    cJSON **at = malloc(sizeof *at * (size_t)(n ? n : 1));
    if (!group || !at) {
        free(group);
        free(at);
        return;
    }
    for (int i = 0; i < n; i++) {
        at[i] = cJSON_GetArrayItem(items, i);
        group[i] = i;
    }
    /* Each shown Switch entry joins the group of the first one before it that
     * it goes with. */
    for (int i = 0; i < n; i++) {
        if (!cJSON_GetObjectItemCaseSensitive(at[i], "_sib") || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(at[i], "not_game")))
            continue;
        for (int j = 0; j < i; j++)
            if (cJSON_GetObjectItemCaseSensitive(at[j], "_sib") &&
                !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(at[j], "not_game")) && nx_same(at[i], at[j])) {
                group[i] = group[j];
                break;
            }
    }
    for (int g = 0; g < n; g++) {
        int best = -1, members = 0;
        for (int i = 0; i < n; i++)
            if (group[i] == g) {
                members++;
                if (best < 0 || nx_rank(at[i]) > nx_rank(at[best])) best = i;
            }
        if (members < 2) continue;
        for (int i = 0; i < n; i++)
            if (group[i] == g && i != best) {
                cJSON_DeleteItemFromObjectCaseSensitive(at[i], "not_game");
                cJSON_AddBoolToObject(at[i], "not_game", 1);
                cJSON_AddNumberToObject(at[i], "same_as", jget_num(at[best], "id", 0));
            }
    }
    for (int i = 0; i < n; i++) {
        cJSON_DeleteItemFromObjectCaseSensitive(at[i], "_game");
        cJSON_DeleteItemFromObjectCaseSensitive(at[i], "_sib");
    }
    free(group);
    free(at);
}

/* The pages either side of the one just shown, fetched in the background so
 * scrolling on (or back up, after a jump to a letter) finds them here. RomM
 * takes a second or two for a page of a big platform. One thread, working
 * from the latest page shown: what was asked for meanwhile is dropped. */
static struct {
    int busy, pending, offset, limit, total;
    library_scope scope;
    char path[sizeof g_pages[0].key], search[256];
    pthread_t thread;
} g_near;

static int page_cached(const char *path, int offset, int limit) {
    char key[sizeof g_pages[0].key];
    snprintf(key, sizeof key, "%s&offset=%d&limit=%d", path, offset, limit);
    int hit = 0;
    double now = mono_now();
    pthread_mutex_lock(&g_pages_lock);
    for (int i = 0; i < PAGE_CACHE_SLOTS && !hit; i++)
        hit = g_pages[i].page && !strcmp(g_pages[i].key, key) && now - g_pages[i].at < PAGE_CACHE_SEC / 2;
    pthread_mutex_unlock(&g_pages_lock);
    return hit;
}

static void *near_pages_thread(void *arg) {
    (void)arg;
    char err[160], path[sizeof g_near.path], search[sizeof g_near.search];
    pthread_mutex_lock(&g_pages_lock);
    g_near.thread = pthread_self();
    while (g_near.pending) {
        g_near.pending = 0;
        library_scope scope = g_near.scope;
        int offset = g_near.offset, limit = g_near.limit, total = g_near.total;
        str_copy(path, sizeof path, g_near.path);
        str_copy(search, sizeof search, g_near.search);
        pthread_mutex_unlock(&g_pages_lock);
        /* Down first, two pages: that's the way most scrolling goes. */
        const int near[3] = {offset + limit, offset + 2 * limit, offset - limit};
        for (int k = 0; k < 3; k++) {
            if (near[k] < 0 || near[k] >= total || page_cached(path, near[k], limit)) continue;
            cJSON_Delete(library_roms(&scope, search, near[k], limit, err, sizeof err));
            pthread_mutex_lock(&g_pages_lock);
            int moved = g_near.pending;
            pthread_mutex_unlock(&g_pages_lock);
            if (moved) break;
        }
        pthread_mutex_lock(&g_pages_lock);
    }
    g_near.busy = 0;
    memset(&g_near.thread, 0, sizeof g_near.thread);
    pthread_mutex_unlock(&g_pages_lock);
    return NULL;
}

static void fetch_near_pages(const char *path, const library_scope *scope, const char *search, int offset, int limit,
                             int total) {
    if (!(offset + limit < total && !page_cached(path, offset + limit, limit)) &&
        !(offset - limit >= 0 && !page_cached(path, offset - limit, limit)))
        return;
    pthread_mutex_lock(&g_pages_lock);
    if (g_near.busy && pthread_equal(g_near.thread, pthread_self())) {
        pthread_mutex_unlock(&g_pages_lock); /* its own fetches don't ask for more */
        return;
    }
    g_near.pending = 1;
    g_near.scope = *scope;
    g_near.offset = offset;
    g_near.limit = limit;
    g_near.total = total;
    str_copy(g_near.path, sizeof g_near.path, path);
    str_copy(g_near.search, sizeof g_near.search, search ? search : "");
    int start = !g_near.busy;
    g_near.busy = 1;
    pthread_mutex_unlock(&g_pages_lock);
    if (start && thread_start(near_pages_thread, NULL) != 0) {
        pthread_mutex_lock(&g_pages_lock);
        g_near.busy = 0;
        pthread_mutex_unlock(&g_pages_lock);
    }
}

/* A platform's versions of a game come as one entry (RomM groups them by their
 * metadata), except on the Switch, where an update or DLC can share its game's
 * metadata and nx_collapse sorts the entries out instead. Asked once a
 * platform; a search across every platform is grouped. */
static int group_versions(int platform_id) {
    static struct { int id, group; } seen[64];
    static int count;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (platform_id <= 0) return 1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < count; i++)
        if (seen[i].id == platform_id) {
            const int group = seen[i].group;
            pthread_mutex_unlock(&lock);
            return group;
        }
    pthread_mutex_unlock(&lock);
    char path[64];
    snprintf(path, sizeof path, "/api/platforms/%d", platform_id);
    long st = 0;
    cJSON *p = romm_call("GET", path, NULL, &st);
    if (st != 200 || !p) { /* can't tell: one card per entry, as before */
        cJSON_Delete(p);
        return 0;
    }
    const int group = !is_switch(jget_str(p, "slug", ""));
    cJSON_Delete(p);
    pthread_mutex_lock(&lock);
    if (count < 64) seen[count].id = platform_id, seen[count].group = group, count++;
    pthread_mutex_unlock(&lock);
    return group;
}

cJSON *library_roms(const library_scope *scope, const char *search, int offset, int limit, char *err, int en) {
    char path[3072], filter[2048] = "";
    int o = 0;
    /* Platform 0: every platform (a search across the library, or a
     * collection's games). Games whose files RomM's last scan didn't find are
     * left out: they can't come down. */
    if (scope->platform_id > 0) o += snprintf(filter, sizeof filter, "&platform_ids=%d", scope->platform_id);
    if (scope->collection != COLLECTION_NONE) {
        if (!library_collection_id_ok(scope->collection, scope->collection_id)) {
            snprintf(err, (size_t)en, "no such collection");
            return NULL;
        }
        char *id = http_escape(scope->collection_id);
        o += snprintf(filter + o, sizeof filter - (size_t)o, "&%s=%s", g_collection_kinds[scope->collection].rom_param, id);
        free(id);
    }
    /* Not in a collection: RomM fronts a game's group with one version, and a
     * collection holding others (Square USA: Final Fantasy IX's discs, not
     * its .m3u) would lose the game. A collection lists what's in it. */
    if (group_versions(scope->platform_id) && scope->collection == COLLECTION_NONE)
        snprintf(filter + o, sizeof filter - (size_t)o, "&group_by_meta_id=true");
    char *q = http_escape(search ? search : "");
    snprintf(path, sizeof path, "/api/roms?search_term=%s%s&order_by=name&order_dir=asc&missing=false" ROMS_LEAN, q, filter);
    free(q);
    char key[sizeof g_pages[0].key];
    snprintf(key, sizeof key, "%s&offset=%d&limit=%d", path, offset, limit);
    cJSON *out = page_cache_get(key), *it;
    if (out) {
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(out, "items"))
            cJSON_ReplaceItemInObjectCaseSensitive(it, "installed", cJSON_CreateBool(any_on_console(it)));
        fetch_near_pages(path, scope, search, offset, limit, (int)jget_num(out, "total", 0));
        return out;
    }
    out = cJSON_CreateObject();
    cJSON_AddArrayToObject(out, "items");
    long st = 0;
    if (romm_list(path, offset, limit, add_library_rom, out, &st) < 0) {
        if (st == 200) snprintf(err, (size_t)en, "could not read RomM's list of games (see the log)");
        else if (st || !romm_no_answer(err, en)) snprintf(err, (size_t)en, "could not load games (HTTP %ld)", st);
        cJSON_Delete(out);
        return NULL;
    }
    if (!cJSON_GetObjectItemCaseSensitive(out, "total")) cJSON_AddNumberToObject(out, "total", 0);
    nx_collapse(out);
    page_cache_put(key, out);
    fetch_near_pages(path, scope, search, offset, limit, (int)jget_num(out, "total", 0));
    return out;
}
