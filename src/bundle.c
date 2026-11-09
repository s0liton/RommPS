#include "bundle.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gameid.h"
#include "platform.h"
#include "ps2card.h"
#include "xboxhdd.h"
#include "zip.h"

int bundle_supported(const char *layout) {
    return layout && (!strcmp(layout, "psp") || !strcmp(layout, "ps3") || !strcmp(layout, "gci") || !strcmp(layout, "wii") ||
                      !strcmp(layout, "3ds") || !strcmp(layout, "wiiu") || !strcmp(layout, "ps2") ||
            !strcmp(layout, "switch") || !strcmp(layout, "xbox") || !strcmp(layout, "x360") || !strcmp(layout, "psvita") ||
            !strcmp(layout, "ps4") || !strcmp(layout, "folder") || !strcmp(layout, "scummvm") ||
            !strcmp(layout, "ryujinx"));
}

/* psvita, ps4, folder: a game's saves are the one folder its id names. */
static int one_folder(const char *layout) {
    return !strcmp(layout, "psvita") || !strcmp(layout, "ps4") || !strcmp(layout, "folder");
}

/* scummvm: one of the target's save files, "<target>.<number>" (most
 * engines: monkey2.000) or "<target>.s<number>" (SCUMM's: monkey.s01). */
static int scummvm_save(const char *name, const char *target) {
    const size_t tl = strlen(target);
    if (strncasecmp(name, target, tl) != 0 || name[tl] != '.') return 0;
    const char *n = name + tl + 1;
    if (*n == 's' || *n == 'S') n++;
    return *n && strspn(n, "0123456789") == strlen(n);
}

static void add(bundle *b, const char *path, const char *name) {
    if (b->n == b->cap) {
        const int want = b->cap ? b->cap * 2 : 16;
        void *grown = realloc(b->files, sizeof *b->files * (size_t)want);
        if (!grown) {
            LOGE("no memory for more files in %s's save: %s is left out", b->dir, name);
            return;
        }
        b->files = grown;
        b->cap = want;
    }
    str_copy(b->files[b->n].path, sizeof b->files[0].path, path);
    str_copy(b->files[b->n].name, sizeof b->files[0].name, name);
    b->n++;
    struct stat st;
    if (stat(path, &st) == 0) {
        if (st.st_mtime > b->mtime) b->mtime = st.st_mtime;
        b->size += st.st_size;
    }
}

/* Every file below dir, named prefix/relative/path. */
static void add_tree(bundle *b, const char *dir, const char *prefix, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN], name[512];
        path_join(path, sizeof path, dir, de->d_name);
        if (prefix[0])
            snprintf(name, sizeof name, "%s/%s", prefix, de->d_name);
        else
            str_copy(name, sizeof name, de->d_name);
        if (dir_exists(path)) {
            if (depth < 8) add_tree(b, path, name, depth + 1);
        } else {
            add(b, path, name);
        }
    }
    closedir(d);
}

/* PPSSPP dumps game data installs into SAVEDATA too, because why not. Those
 * have a PARAM.SFO without save fields, so skip them like the other RomM clients do. */
/* A PSP save is a few KB to a couple of MB. Games also keep what they install
 * to the memory stick (Peace Walker: 848 MB of "Installed Data") and their
 * downloaded content in SAVEDATA, made the same way: that's game data, and
 * RomM wouldn't take it anyway. */
#define PSP_SAVE_MAX (16 << 20)

static int64_t tree_size(const char *dir, int64_t stop) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int64_t total = 0;
    while (d && total <= stop && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[PATH_MAX_LEN];
        path_join(p, sizeof p, dir, de->d_name);
        const int64_t n = dir_exists(p) ? tree_size(p, stop - total) : file_size(p);
        if (n > 0) total += n;
    }
    if (d) closedir(d);
    return total;
}

static int is_psp_save(const char *folder) {
    char sfo[PATH_MAX_LEN], v[64];
    if (tree_size(folder, PSP_SAVE_MAX) > PSP_SAVE_MAX) {
        LOGD("%s: game data, not a save (over %d MB)", folder, PSP_SAVE_MAX >> 20);
        return 0;
    }
    path_join(sfo, sizeof sfo, folder, "PARAM.SFO");
    if (!file_exists(sfo)) return 1;
    return sfo_file_get(sfo, "SAVEDATA_PARAMS", v, sizeof v) == 0 || sfo_file_get(sfo, "SAVEDATA_FILE_LIST", v, sizeof v) == 0;
}

/* Dolphin's region folder for a game: USA, JAP or EUR, unless the folder
 * has RetroDECK's shorter US, JP or EU instead. */
static const char *gc_region(const char *base, char r) {
    const char *full, *brief;
    switch (r) {
    case 'E': full = "USA", brief = "US"; break;
    case 'J': case 'K': case 'W': full = "JAP", brief = "JP"; break;
    default: full = "EUR", brief = "EU"; break;
    }
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, base, full);
    if (dir_exists(p)) return full;
    path_join(p, sizeof p, base, brief);
    return dir_exists(p) ? brief : full;
}

static void wii_title_hex(const char *id, char *out) {
    for (int i = 0; i < 4; i++) sprintf(out + i * 2, "%02x", (unsigned char)id[i]);
}

/* A 3DS or Wii U title id, "0004000000066C00": its halves in lowercase. */
static int title_halves(const char *id, char hi[9], char lo[9]) {
    if (strlen(id) != 16) return -1;
    for (int i = 0; i < 16; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return -1;
        (i < 8 ? hi : lo)[i % 8] = (char)(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
    }
    hi[8] = lo[8] = 0;
    return 0;
}

/* Cemu's account on this console, from settings.xml beside mlc01 ("<base>"
 * is mlc01/usr/save): <PersistentId>2147483649</PersistentId> is 80000001. */
static void wiiu_account(const char *base, char out[16]) {
    char cfg[PATH_MAX_LEN];
    str_copy(cfg, sizeof cfg, base);
    for (int k = 0; k < 3; k++) {
        char *p = strrchr(cfg, '/');
        if (p) *p = 0;
    }
    str_copy(out, 16, "80000001");
    char path[PATH_MAX_LEN];
    path_join(path, sizeof path, cfg, "settings.xml");
    char *xml = read_file(path, NULL);
    const char *at = xml ? strstr(xml, "<PersistentId>") : NULL;
    unsigned long v = at ? strtoul(at + 14, NULL, 10) : 0;
    if (v >= 0x80000000UL && v <= 0xFFFFFFFFUL) snprintf(out, 16, "%08lx", v);
    free(xml);
}

/* The game's save, copied out of the Xbox hard disk image into b->dir. The
 * copy is made again only when the image has changed since: ".image" in it
 * says which version it came from. No image (""): the copy as it is. */
static void xbox_copy_out(bundle *b, const char *image) {
    char stamp[PATH_MAX_LEN], now[64];
    snprintf(b->dir, sizeof b->dir, "%s/xbox/%s", plat_data_dir(), b->game_id);
    path_join(stamp, sizeof stamp, b->dir, ".image");
    snprintf(now, sizeof now, "%lld %lld", (long long)file_mtime_ns(image), (long long)file_size(image));
    char *had = read_file(stamp, NULL);
    const int fresh = had && !strcmp(had, now);
    free(had);
    if (fresh || !image[0] || file_size(image) < 0) return;
    remove_tree(b->dir);
    xhdd *h = xhdd_open(image, 0);
    const int rc = h ? xhdd_get_save(h, b->game_id, b->dir) : -1;
    xhdd_close(h);
    if (rc < 0) {
        remove_tree(b->dir); /* tried again next time */
        return;
    }
    mkdir_p(b->dir);
    write_file_atomic(stamp, now, strlen(now));
}

/* psp, ps3: whether a save folder is the game's, by the title id it starts with. */
static int folder_is_games(const bundle *b, const char *name) {
    if (!strncasecmp(name, b->game_id, strlen(b->game_id))) return 1;
    char list[sizeof b->also], *save = NULL;
    str_copy(list, sizeof list, b->also);
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save))
        if (!strncasecmp(name, t, strlen(t))) return 1;
    return 0;
}

/* "BLUS30190" out of "BLUS30190MC4_SAV": four letters, five digits, the
 * shape of every PSP and PS3 title id. -1 for anything else. */
static int title_id_of(const char *name, char out[10]) {
    for (int i = 0; i < 9; i++)
        if (i < 4 ? !isalpha((unsigned char)name[i]) : !isdigit((unsigned char)name[i])) return -1;
    for (int i = 0; i < 9; i++) out[i] = (char)toupper((unsigned char)name[i]);
    out[9] = 0;
    return 0;
}

/* The user every yuzu fork keeps its device saves under. */
#define NX_DEVICE_USER "00000000000000000000000000000000"

static uint64_t le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Ryujinx's save index (LibHac's KeyValueArchive, imkvdb.arc): "IMKV", its
 * entry count at 8, then entries of "IMEN", the key's and value's sizes, the
 * key (a SaveDataAttribute: program id, the user's two halves at 8 and 16,
 * the type at 0x20, 1 a user's save and 3 the device's) and the value (the
 * save's folder id first; its space at 0x18, 1 the user's; its state at 0x19,
 * 3 deleted). The game's save for user (as Profiles.json writes the id: the
 * two halves in hex) and its device save: their folder ids, 0 for none. As
 * argosy-sigil's load_index reads it. */
static void ryujinx_saves(const char *root, const char *title, const char *user, uint64_t *account, uint64_t *device) {
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof path, "%s/bis/system/save/8000000000000000/0/imkvdb.arc", root);
    size_t len = 0;
    unsigned char *d = (unsigned char *)read_file(path, &len);
    const uint64_t program = strtoull(title, NULL, 16);
    *account = *device = 0;
    if (!d || len < 12 || le32(d) != 0x564B4D49u || !program) {
        free(d);
        return;
    }
    const uint32_t count = le32(d + 8);
    size_t at = 12;
    for (uint32_t i = 0; i < count && at + 12 <= len && le32(d + at) == 0x4E454D49u; i++) {
        const uint32_t kl = le32(d + at + 4), vl = le32(d + at + 8);
        if (kl > len || vl > len || at + 12 + kl + vl > len) break;
        const unsigned char *key = d + at + 12, *value = key + kl;
        at += 12 + (size_t)kl + vl;
        if (kl < 0x40 || vl < 0x1A || le64(key) != program || value[0x18] != 1 || value[0x19] == 3) continue;
        char whose[40];
        snprintf(whose, sizeof whose, "%016llx%016llx", (unsigned long long)le64(key + 8), (unsigned long long)le64(key + 16));
        if (key[0x20] == 1 && !strcasecmp(whose, user)) *account = le64(value);
        else if (key[0x20] == 3) *device = le64(value);
    }
    free(d);
}

int bundle_open(bundle *b, const char *layout, const char *id, const char *also, char bases[][PATH_MAX_LEN], int nbases) {
    memset(b, 0, sizeof *b);
    if (!bundle_supported(layout) || !id[0] || nbases < 1) return -1;
    str_copy(b->layout, sizeof b->layout, layout);
    str_copy(b->game_id, sizeof b->game_id, id);
    if (also) str_copy(b->also, sizeof b->also, also);

    if (!strcmp(layout, "ps2")) {
        /* The slot cards: the game's save folders on a folder card, or a file
         * card holding any of its saves. */
        str_copy(b->dir, sizeof b->dir, bases[0]);
        str_copy(b->base, sizeof b->base, bases[0]);
        char *slash = strrchr(b->base, '/');
        if (slash) *slash = 0;
        for (int i = 0; i < nbases; i++) {
            const char *card = path_basename(bases[i]);
            if (!dir_exists(bases[i])) {
                if (file_exists(bases[i]) && ps2card_file_has(bases[i], id)) add(b, bases[i], card);
                continue;
            }
            DIR *d = opendir(bases[i]);
            struct dirent *de;
            while (d && (de = readdir(d))) {
                char folder[PATH_MAX_LEN], name[512];
                path_join(folder, sizeof folder, bases[i], de->d_name);
                if (!ps2card_folder_is_game(de->d_name, id) || !dir_exists(folder)) continue;
                snprintf(name, sizeof name, "%s/%s", card, de->d_name);
                add_tree(b, folder, name, 0);
            }
            if (d) closedir(d);
        }
        return 0;
    }

    int pick = 0;
    for (int i = 0; i < nbases; i++)
        if (dir_exists(bases[i])) {
            pick = i;
            break;
        }
    const char *base = bases[pick];
    str_copy(b->base, sizeof b->base, base);

    if (!strcmp(layout, "psp") || !strcmp(layout, "ps3")) {
        /* ps3: RPCS3's savedata, each folder a game writes starting with
         * its title id. Its installs are in dev_hdd0/game, not here, so
         * there's no size to tell them apart by. */
        const int psp = !strcmp(layout, "psp");
        str_copy(b->dir, sizeof b->dir, base);
        DIR *d = opendir(base);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            char folder[PATH_MAX_LEN];
            path_join(folder, sizeof folder, base, de->d_name);
            if (!folder_is_games(b, de->d_name) || !dir_exists(folder) || (psp && !is_psp_save(folder))) continue;
            add_tree(b, folder, de->d_name, 0);
        }
        if (d) closedir(d);
    } else if (!strcmp(layout, "gci")) {
        if (strlen(id) < 6) return -1;
        snprintf(b->dir, sizeof b->dir, "%s/%s/Card A", base, gc_region(base, id[3]));
        DIR *d = opendir(b->dir);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!str_ends_with_ci(de->d_name, ".gci")) continue;
            char path[PATH_MAX_LEN];
            unsigned char hdr[6];
            path_join(path, sizeof path, b->dir, de->d_name);
            FILE *f = fopen(path, "rb");
            int ok = f && fread(hdr, 1, 6, f) == 6 && !memcmp(hdr, id, 6);
            if (f) fclose(f);
            if (ok) add(b, path, de->d_name);
        }
        if (d) closedir(d);
    } else if (!strcmp(layout, "xbox")) {
        /* E:\UDATA\<id> inside the image (base), hashed and zipped from a copy. */
        if (strlen(id) != 8) return -1;
        xbox_copy_out(b, base);
        add_tree(b, b->dir, "", 0);
    } else if (!strcmp(layout, "x360")) {
        /* <title id>/00000001 (saved games) and its headers under the player's
         * folder; the title's other content (DLC, updates) stays out. */
        char dir[PATH_MAX_LEN], name[64];
        snprintf(b->dir, sizeof b->dir, "%s/%s", base, id);
        snprintf(dir, sizeof dir, "%s/00000001", b->dir);
        snprintf(name, sizeof name, "%s/00000001", id);
        add_tree(b, dir, name, 0);
        snprintf(dir, sizeof dir, "%s/Headers/00000001", b->dir);
        snprintf(name, sizeof name, "%s/Headers/00000001", id);
        add_tree(b, dir, name, 0);
    } else if (one_folder(layout)) {
        /* The one folder the game's saves are in, in whatever case it's there:
         * Vita3K's ux0:user/00/savedata/<save folder> (gameid_vita), shadPS4's
         * savedata/<title id> (its save folders in it), MAME's nvram/<set>. */
        DIR *d = opendir(base);
        struct dirent *de;
        snprintf(b->dir, sizeof b->dir, "%s/%s", base, id);
        while (d && (de = readdir(d)))
            if (!strcasecmp(de->d_name, id)) path_join(b->dir, sizeof b->dir, base, de->d_name);
        if (d) closedir(d);
        add_tree(b, b->dir, id, 0);
    } else if (!strcmp(layout, "scummvm")) {
        /* ScummVM's save folder, every game's saves side by side: this target's. */
        str_copy(b->dir, sizeof b->dir, base);
        DIR *d = opendir(base);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            char path[PATH_MAX_LEN];
            path_join(path, sizeof path, base, de->d_name);
            if (scummvm_save(de->d_name, id) && file_exists(path)) add(b, path, de->d_name);
        }
        if (d) closedir(d);
    } else if (!strcmp(layout, "switch")) {
        /* The game's folder under the user's: save/0000000000000000/<user>/<title id>;
         * its device save beside it, under the all-zero user. */
        snprintf(b->dir, sizeof b->dir, "%s/%s", base, id);
        add_tree(b, b->dir, id, 0);
        char up[PATH_MAX_LEN], name[64];
        str_copy(up, sizeof up, base);
        char *slash = strrchr(up, '/');
        if (slash && strcmp(slash + 1, NX_DEVICE_USER) != 0) {
            *slash = 0;
            snprintf(b->dir2, sizeof b->dir2, "%s/" NX_DEVICE_USER "/%s", up, id);
            snprintf(name, sizeof name, "device/%s", id);
            add_tree(b, b->dir2, name, 0);
        }
    } else if (!strcmp(layout, "ryujinx")) {
        uint64_t account = 0, device = 0;
        char root[PATH_MAX_LEN], name[64];
        const char *user = path_basename(base);
        str_copy(root, sizeof root, base);
        const size_t rl = strlen(root), tail = strlen("/bis/user/save/") + strlen(user);
        if (rl <= tail || strncmp(root + rl - tail, "/bis/user/save/", 15) != 0) return -1;
        root[rl - tail] = 0;
        ryujinx_saves(root, id, user, &account, &device);
        if (account) {
            snprintf(b->dir, sizeof b->dir, "%s/bis/user/save/%016llx/0", root, (unsigned long long)account);
            add_tree(b, b->dir, id, 0);
        }
        if (device) {
            snprintf(b->dir2, sizeof b->dir2, "%s/bis/user/save/%016llx/0", root, (unsigned long long)device);
            snprintf(name, sizeof name, "device/%s", id);
            add_tree(b, b->dir2, name, 0);
        }
    } else if (!strcmp(layout, "3ds")) {
        char hi[9], lo[9], root[64], meta[PATH_MAX_LEN];
        if (title_halves(id, hi, lo) != 0) return -1;
        snprintf(b->dir, sizeof b->dir, "%s/%s/%s/data/00000001", base, hi, lo);
        snprintf(root, sizeof root, "%s/%s/data/00000001", hi, lo);
        add_tree(b, b->dir, root, 0);
        snprintf(meta, sizeof meta, "%s.metadata", b->dir);
        snprintf(root, sizeof root, "%s/%s/data/00000001.metadata", hi, lo);
        if (b->n && file_exists(meta)) add(b, meta, root);
    } else if (!strcmp(layout, "wiiu")) {
        char hi[9], lo[9], dir[PATH_MAX_LEN], name[64];
        if (title_halves(id, hi, lo) != 0) return -1;
        wiiu_account(base, b->account);
        snprintf(b->dir, sizeof b->dir, "%s/%s/%s", base, hi, lo);
        snprintf(dir, sizeof dir, "%s/user/%s", b->dir, b->account);
        snprintf(name, sizeof name, "%s/user/account", lo);
        add_tree(b, dir, name, 0);
        snprintf(dir, sizeof dir, "%s/user/common", b->dir);
        snprintf(name, sizeof name, "%s/user/common", lo);
        add_tree(b, dir, name, 0);
        if (b->n) { /* the title's meta only goes with a save */
            snprintf(dir, sizeof dir, "%s/meta", b->dir);
            snprintf(name, sizeof name, "%s/meta", lo);
            add_tree(b, dir, name, 0);
        }
    } else {
        char hex[9];
        if (strlen(id) < 4) return -1;
        wii_title_hex(id, hex);
        snprintf(b->dir, sizeof b->dir, "%s/title/00010000/%s", base, hex);
        add_tree(b, b->dir, hex, 0);
    }
    return 0;
}

void bundle_free(bundle *b) {
    free(b->files);
    b->files = NULL;
    b->n = b->cap = 0;
}

static int cmp_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int bundle_hash(const bundle *b, char out[33]) {
    char **lines = calloc((size_t)b->n + 1, sizeof *lines);
    size_t total = 1;
    for (int i = 0; i < b->n; i++) {
        char h[33];
        if (md5_file_hex(b->files[i].path, h) != 0) h[0] = 0;
        size_t l = strlen(b->files[i].name) + 34;
        lines[i] = malloc(l);
        snprintf(lines[i], l, "%s:%s", b->files[i].name, h);
        total += l;
    }
    qsort(lines, (size_t)b->n, sizeof *lines, cmp_lines);
    char *joined = malloc(total), *p = joined;
    *p = 0;
    for (int i = 0; i < b->n; i++) p += sprintf(p, i ? "\n%s" : "%s", lines[i]);
    md5_hex(joined, strlen(joined), out);
    for (int i = 0; i < b->n; i++) free(lines[i]);
    free(lines);
    free(joined);
    return 0;
}

int bundle_zip(const bundle *b, const char *zip_path) {
    zip_member *m = calloc((size_t)b->n + 1, sizeof *m);
    for (int i = 0; i < b->n; i++) {
        m[i].path = b->files[i].path;
        m[i].name = b->files[i].name;
    }
    int rc = zip_write(zip_path, m, b->n);
    free(m);
    return rc;
}

/* The folder named name somewhere below dir (at most depth levels down),
 * whose parent is called parent when that's given. */
static int find_dir(const char *dir, const char *name, const char *parent, int depth, char *out, size_t n) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int found = 0;
    while (d && !found && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        if (!dir_exists(path)) continue;
        if (!strcasecmp(de->d_name, name) && (!parent || !strcasecmp(path_basename(dir), parent))) {
            str_copy(out, n, path);
            found = 1;
        } else if (depth > 0) {
            found = find_dir(path, name, parent, depth - 1, out, n) == 0;
        }
    }
    if (d) closedir(d);
    return found ? 0 : -1;
}

/* Puts the folder from where to (replacing to), across drives too. */
static int put_folder(const char *from, const char *to) {
    remove_tree(to);
    if (mkdir_parent(to) != 0) return -1;
    if (rename(from, to) == 0) return 0;
    return link_or_copy(from, to);
}

/* The save inside an unpacked zip, in whatever form it came. */
static int place_unpacked(bundle *b, const char *tmp) {
    char src[PATH_MAX_LEN], hex[9];
    if (!strcmp(b->layout, "wii")) {
        /* "<id>/..." (RomM's save units), or the folder's contents as 1.2 sent them. */
        wii_title_hex(b->game_id, hex);
        path_join(src, sizeof src, tmp, hex);
        return put_folder(dir_exists(src) ? src : tmp, b->dir);
    }
    if (!strcmp(b->layout, "xbox")) {
        /* "<title id>/..." in any case, or the folder's contents; onto the
         * image, and the copy here goes so it's made again. */
        DIR *d = opendir(tmp);
        struct dirent *de;
        str_copy(src, sizeof src, tmp);
        while (d && (de = readdir(d)))
            if (!strcasecmp(de->d_name, b->game_id)) path_join(src, sizeof src, tmp, de->d_name);
        if (d) closedir(d);
        const int rc = xhdd_put_save_file(b->base, b->game_id, src);
        remove_tree(b->dir);
        return rc;
    }
    if (!strcmp(b->layout, "x360")) {
        /* "<title id>/00000001" and "<title id>/Headers/00000001" (any case),
         * or the title folder's contents; the title's other content stays. */
        str_copy(src, sizeof src, tmp);
        DIR *d = opendir(tmp);
        struct dirent *de;
        while (d && (de = readdir(d)))
            if (!strcasecmp(de->d_name, b->game_id)) path_join(src, sizeof src, tmp, de->d_name);
        if (d) closedir(d);
        char saves[PATH_MAX_LEN], headers[PATH_MAX_LEN], to[PATH_MAX_LEN];
        path_join(saves, sizeof saves, src, "00000001");
        snprintf(headers, sizeof headers, "%s/Headers/00000001", src);
        if (!dir_exists(saves)) return -1;
        snprintf(to, sizeof to, "%s/00000001", b->dir);
        int rc = put_folder(saves, to);
        snprintf(to, sizeof to, "%s/Headers/00000001", b->dir);
        if (dir_exists(headers)) rc |= put_folder(headers, to);
        else remove_tree(to);
        return rc;
    }
    if (one_folder(b->layout)) {
        /* "<id>/..." in any case (argosy-sigil's vita3k unit, and the same
         * shape for the others); for psvita and ps4, one folder alone under
         * another title id (a client that went by TITLE_ID where the game
         * saves under another's); or the folder's contents. A folder named
         * like another game's id (or, for MAME, any lone folder but the
         * set's) isn't this game's: nothing is touched. */
        const int titles = strcmp(b->layout, "folder") != 0;
        DIR *d = opendir(tmp);
        struct dirent *de;
        char only[PATH_MAX_LEN] = "", tid[10];
        int entries = 0, dirs = 0, foreign = 0, exact = 0;
        str_copy(src, sizeof src, tmp);
        while (d && (de = readdir(d))) {
            char in[PATH_MAX_LEN];
            if (de->d_name[0] == '.') continue;
            path_join(in, sizeof in, tmp, de->d_name);
            entries++;
            if (!dir_exists(in)) continue;
            dirs++;
            str_copy(only, sizeof only, in);
            if (!strcasecmp(de->d_name, b->game_id)) {
                str_copy(src, sizeof src, in);
                exact = 1;
            } else if (titles && strlen(de->d_name) == 9 && title_id_of(de->d_name, tid) == 0) {
                foreign++;
            }
        }
        if (d) closedir(d);
        if (!exact && titles && entries == 1 && foreign == 1) str_copy(src, sizeof src, only);
        else if (!exact && (foreign || (!titles && entries == 1 && dirs == 1))) return -2;
        return put_folder(src, b->dir);
    }
    if (!strcmp(b->layout, "scummvm")) {
        /* The target's save files, at the zip's top or in one folder: they
         * replace the target's saves here, whole. */
        DIR *d = opendir(tmp);
        struct dirent *de;
        char from[PATH_MAX_LEN];
        int any = 0;
        str_copy(from, sizeof from, tmp);
        while (d && (de = readdir(d))) {
            char in[PATH_MAX_LEN];
            path_join(in, sizeof in, tmp, de->d_name);
            if (de->d_name[0] != '.' && dir_exists(in)) str_copy(from, sizeof from, in);
            any |= scummvm_save(de->d_name, b->game_id);
        }
        if (d) closedir(d);
        if (any) str_copy(from, sizeof from, tmp);
        d = opendir(from);
        int rc = 0, got = 0;
        while (d && (de = readdir(d))) {
            if (!scummvm_save(de->d_name, b->game_id)) continue;
            if (!got++)
                for (int i = 0; i < b->n; i++) unlink(b->files[i].path);
            char in[PATH_MAX_LEN], to[PATH_MAX_LEN];
            path_join(in, sizeof in, from, de->d_name);
            path_join(to, sizeof to, b->dir, de->d_name);
            if (mkdir_parent(to) != 0 || (rename(in, to) != 0 && link_or_copy(in, to) != 0)) rc = -1;
        }
        if (d) closedir(d);
        return got ? rc : -2;
    }
    if (!strcmp(b->layout, "switch") || !strcmp(b->layout, "ryujinx")) {
        /* "<title id>/..." and "device/<title id>/...", or the folder's contents.
         * A device save with no folder here (Ryujinx made none) stays out. */
        char dev[PATH_MAX_LEN], devs[PATH_MAX_LEN];
        int rc = 0, any = 0;
        path_join(devs, sizeof devs, tmp, "device");
        path_join(dev, sizeof dev, devs, b->game_id);
        const int had_dev = dir_exists(dev);
        if (had_dev && b->dir2[0]) {
            rc |= put_folder(dev, b->dir2);
            any = 1;
        }
        remove_tree(devs);
        path_join(src, sizeof src, tmp, b->game_id);
        if (b->dir[0] && (dir_exists(src) || !had_dev)) {
            rc |= put_folder(dir_exists(src) ? src : tmp, b->dir);
            any = 1;
        }
        return any ? rc : -2;
    }
    if (!strcmp(b->layout, "3ds")) {
        /* data/00000001 wherever it is: "<hi>/<lo>/data/00000001", "data/00000001"
         * or the whole path from the SD card. */
        if (find_dir(tmp, "00000001", "data", 8, src, sizeof src) != 0) return -1;
        char meta[PATH_MAX_LEN + 16], local_meta[PATH_MAX_LEN + 16];
        snprintf(meta, sizeof meta, "%s.metadata", src);
        snprintf(local_meta, sizeof local_meta, "%s.metadata", b->dir);
        if (put_folder(src, b->dir) != 0) return -1;
        if (file_exists(meta)) {
            unlink(local_meta);
            if (rename(meta, local_meta) != 0) link_or_copy(meta, local_meta);
        }
        return 0;
    }
    /* wiiu: "<lo>/...", or the save folder's contents, or Cemu's export (an
     * account's files and a cemu_meta file). The account's files go to this
     * console's account; meta stays as it is unless the zip has it. */
    char root[PATH_MAX_LEN], hi[9], lo[9], cemu_meta[PATH_MAX_LEN];
    if (title_halves(b->game_id, hi, lo) != 0) return -1;
    path_join(root, sizeof root, tmp, lo);
    if (!dir_exists(root)) str_copy(root, sizeof root, tmp);
    char user[PATH_MAX_LEN], account[PATH_MAX_LEN], to[PATH_MAX_LEN];
    path_join(user, sizeof user, root, "user");
    path_join(cemu_meta, sizeof cemu_meta, root, "cemu_meta");
    snprintf(to, sizeof to, "%s/user/%s", b->dir, b->account);
    if (!dir_exists(user) && file_exists(cemu_meta)) {
        unlink(cemu_meta);
        return put_folder(root, to);
    }
    path_join(account, sizeof account, user, "account");
    if (!dir_exists(account)) { /* another console's account folder: 8xxxxxxx */
        DIR *d = opendir(user);
        struct dirent *de;
        account[0] = 0;
        while (d && !account[0] && (de = readdir(d)))
            if (de->d_name[0] == '8' && strlen(de->d_name) == 8) path_join(account, sizeof account, user, de->d_name);
        if (d) closedir(d);
    }
    int rc = 0, any = 0;
    if (account[0] && dir_exists(account)) {
        rc |= put_folder(account, to);
        any = 1;
    }
    char common[PATH_MAX_LEN], meta[PATH_MAX_LEN];
    path_join(common, sizeof common, user, "common");
    snprintf(to, sizeof to, "%s/user/common", b->dir);
    if (dir_exists(common)) {
        rc |= put_folder(common, to);
        any = 1;
    }
    path_join(meta, sizeof meta, root, "meta");
    snprintf(to, sizeof to, "%s/meta", b->dir);
    if (any && dir_exists(meta)) rc |= put_folder(meta, to);
    return any ? rc : -1;
}

int bundle_replace(bundle *b, const char *zip_path, const char *tmp_dir) {
    /* Unpack somewhere else first. A bad zip should cost us a download, not
     * someone's 40 hour save. */
    remove_tree(tmp_dir);
    if (zip_extract_all(zip_path, tmp_dir) < 0) {
        remove_tree(tmp_dir);
        return -1;
    }
    int rc;
    if (!strcmp(b->layout, "psp") || !strcmp(b->layout, "ps3") || !strcmp(b->layout, "gci")) {
        if (strcmp(b->layout, "gci") != 0) {
            /* The save folders the zip brings replace theirs, whole. The
             * game's others stay: its downloaded content (which may not be
             * there to download again), what it installed, another slot. */
            DIR *d = opendir(tmp_dir);
            struct dirent *de;
            while (d && (de = readdir(d))) {
                char in[PATH_MAX_LEN], path[PATH_MAX_LEN];
                path_join(in, sizeof in, tmp_dir, de->d_name);
                if (de->d_name[0] == '.' || !dir_exists(in)) continue;
                path_join(path, sizeof path, b->dir, de->d_name);
                remove_tree(path);
            }
            if (d) closedir(d);
            remove_tree(tmp_dir);
        } else {
            remove_tree(tmp_dir);
            for (int i = 0; i < b->n; i++) unlink(b->files[i].path);
        }
        mkdir_p(b->dir);
        rc = zip_extract_all(zip_path, b->dir) < 0 ? -1 : 0;
    } else {
        rc = place_unpacked(b, tmp_dir);
        remove_tree(tmp_dir);
    }

    char id[24], base[1][PATH_MAX_LEN], layout[8], also[sizeof b->also];
    str_copy(id, sizeof id, b->game_id);
    str_copy(layout, sizeof layout, b->layout);
    str_copy(base[0], sizeof base[0], b->base);
    str_copy(also, sizeof also, b->also);
    bundle_free(b);
    /* List the files again from the same folder. */
    bundle_open(b, layout, id, also, base, 1);
    return rc;
}

int bundle_other_ids(const bundle *b, const char *zip_path, const char *tmp_dir, char *out, size_t n) {
    out[0] = 0;
    if (strcmp(b->layout, "psp") != 0 && strcmp(b->layout, "ps3") != 0) return 0;
    remove_tree(tmp_dir);
    if (zip_extract_all(zip_path, tmp_dir) < 0) {
        remove_tree(tmp_dir);
        return -1;
    }
    DIR *d = opendir(tmp_dir);
    struct dirent *de;
    size_t o = 0;
    while (d && (de = readdir(d))) {
        char in[PATH_MAX_LEN], tid[10];
        path_join(in, sizeof in, tmp_dir, de->d_name);
        if (de->d_name[0] == '.' || !dir_exists(in) || folder_is_games(b, de->d_name) || title_id_of(de->d_name, tid) != 0)
            continue;
        if (strstr(out, tid) || o + strlen(tid) + 2 > n) continue; /* listed already, or no room */
        o += (size_t)snprintf(out + o, n - o, "%s%s", o ? "," : "", tid);
    }
    if (d) closedir(d);
    remove_tree(tmp_dir);
    return 0;
}
