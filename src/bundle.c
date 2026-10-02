#include "bundle.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gameid.h"
#include "zip.h"

int bundle_supported(const char *layout) {
    return layout && (!strcmp(layout, "psp") || !strcmp(layout, "gci") || !strcmp(layout, "wii"));
}

static void add(bundle *b, const char *path, const char *name) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 16;
        b->files = realloc(b->files, sizeof *b->files * (size_t)b->cap);
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
            if (depth < 4) add_tree(b, path, name, depth + 1);
        } else {
            add(b, path, name);
        }
    }
    closedir(d);
}

/* PPSSPP dumps game data installs into SAVEDATA too, because why not. Those
 * have a PARAM.SFO without save fields, so skip them like the other RomM clients do. */
static int is_psp_save(const char *folder) {
    char sfo[PATH_MAX_LEN], v[64];
    path_join(sfo, sizeof sfo, folder, "PARAM.SFO");
    if (!file_exists(sfo)) return 1;
    return sfo_file_get(sfo, "SAVEDATA_PARAMS", v, sizeof v) == 0 || sfo_file_get(sfo, "SAVEDATA_FILE_LIST", v, sizeof v) == 0;
}

static const char *gc_region(char r) {
    switch (r) {
    case 'E': return "USA";
    case 'J': case 'K': case 'W': return "JAP";
    default: return "EUR";
    }
}

static void wii_title_hex(const char *id, char *out) {
    for (int i = 0; i < 4; i++) sprintf(out + i * 2, "%02x", (unsigned char)id[i]);
}

int bundle_open(bundle *b, const char *layout, const char *id, char bases[][PATH_MAX_LEN], int nbases) {
    memset(b, 0, sizeof *b);
    if (!bundle_supported(layout) || !id[0] || nbases < 1) return -1;
    str_copy(b->layout, sizeof b->layout, layout);
    str_copy(b->game_id, sizeof b->game_id, id);

    int pick = 0;
    for (int i = 0; i < nbases; i++)
        if (dir_exists(bases[i])) {
            pick = i;
            break;
        }
    const char *base = bases[pick];

    if (!strcmp(layout, "psp")) {
        str_copy(b->dir, sizeof b->dir, base);
        DIR *d = opendir(base);
        struct dirent *de;
        size_t il = strlen(id);
        while (d && (de = readdir(d))) {
            char folder[PATH_MAX_LEN];
            path_join(folder, sizeof folder, base, de->d_name);
            if (strncasecmp(de->d_name, id, il) != 0 || !dir_exists(folder) || !is_psp_save(folder)) continue;
            add_tree(b, folder, de->d_name, 0);
        }
        if (d) closedir(d);
    } else if (!strcmp(layout, "gci")) {
        if (strlen(id) < 6) return -1;
        snprintf(b->dir, sizeof b->dir, "%s/%s/Card A", base, gc_region(id[3]));
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
    } else {
        char hex[9];
        if (strlen(id) < 4) return -1;
        wii_title_hex(id, hex);
        snprintf(b->dir, sizeof b->dir, "%s/title/00010000/%s", base, hex);
        add_tree(b, b->dir, "", 0);
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

int bundle_replace(bundle *b, const char *zip_path, const char *tmp_dir) {
    /* Unpack somewhere else first. A bad zip should cost us a download, not
     * someone's 40 hour save. */
    remove_tree(tmp_dir);
    if (zip_extract_all(zip_path, tmp_dir) < 0) {
        remove_tree(tmp_dir);
        return -1;
    }
    remove_tree(tmp_dir);

    if (!strcmp(b->layout, "psp")) {
        /* Remove whole save folders; the zip holds them as top-level folders. */
        for (int i = 0; i < b->n; i++) {
            char folder[PATH_MAX_LEN], *slash;
            str_copy(folder, sizeof folder, b->files[i].name);
            slash = strchr(folder, '/');
            if (!slash) continue;
            *slash = 0;
            char path[PATH_MAX_LEN];
            path_join(path, sizeof path, b->dir, folder);
            remove_tree(path);
        }
    } else if (!strcmp(b->layout, "gci")) {
        for (int i = 0; i < b->n; i++) unlink(b->files[i].path);
    } else {
        remove_tree(b->dir);
    }
    mkdir_p(b->dir);
    int rc = zip_extract_all(zip_path, b->dir) < 0 ? -1 : 0;

    char id[16], dir[PATH_MAX_LEN], layout[8];
    str_copy(id, sizeof id, b->game_id);
    str_copy(layout, sizeof layout, b->layout);
    str_copy(dir, sizeof dir, b->dir);
    bundle_free(b);
    /* List the files again from the same folder. */
    char base[1][PATH_MAX_LEN];
    if (!strcmp(layout, "psp")) {
        str_copy(base[0], sizeof base[0], dir);
    } else if (!strcmp(layout, "gci")) {
        str_copy(base[0], sizeof base[0], dir);
        char *p = strrchr(base[0], '/'); /* strip "/Card A" */
        if (p) *p = 0;
        p = strrchr(base[0], '/'); /* and the region */
        if (p) *p = 0;
    } else {
        str_copy(base[0], sizeof base[0], dir);
        for (int k = 0; k < 3; k++) {
            char *p = strrchr(base[0], '/');
            if (p) *p = 0;
        }
    }
    bundle_open(b, layout, id, base, 1);
    return rc;
}
