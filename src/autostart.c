#include "autostart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"
#include "util.h"

#define MAX_PLACES 8

/* A loader's payload folder: the payload in it and the file that turns it on
 * ("" when the payload's presence is enough). */
typedef struct {
    char elf[PATH_MAX_LEN];
    char flag[PATH_MAX_LEN];
    int present; /* the loader is installed */
} place;

/* "a,b,c" into list, at most max entries. */
static int split(const char *s, char list[][PATH_MAX_LEN], int max) {
    int n = 0;
    while (s && *s && n < max) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len && len < PATH_MAX_LEN) {
            memcpy(list[n], s, len);
            list[n++][len] = 0;
        }
        s = comma ? comma + 1 : NULL;
    }
    return n;
}

static int parent_exists(const char *path) {
    char parent[PATH_MAX_LEN];
    str_copy(parent, sizeof parent, path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0;
    return dir_exists(parent);
}

/* Every loader payload folder, the running loader's first. ROMM_SYNC_AUTOSTART
 * (comma separated, running loader first) replaces them in tests. 0 if the
 * loader can't start payloads at boot. */
static int places(place *p, int max) {
    const plat_info_t *pi = plat_info();
    char dirs[MAX_PLACES][PATH_MAX_LEN];
    int n = 0;
    const char *env = getenv("ROMM_SYNC_AUTOSTART");
    if (env && *env) {
        n = split(env, dirs, MAX_PLACES);
    } else if (pi->autostart_dir) {
        str_copy(dirs[n++], PATH_MAX_LEN, pi->autostart_dir);
        for (int i = 0; pi->autostart_also && pi->autostart_also[i] && n < MAX_PLACES; i++)
            str_copy(dirs[n++], PATH_MAX_LEN, pi->autostart_also[i]);
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        snprintf(p[i].elf, sizeof p[i].elf, "%s/%s", dirs[i], pi->payload);
        p[i].flag[0] = 0;
        if (pi->autostart_flag) snprintf(p[i].flag, sizeof p[i].flag, "%s/%s", dirs[i], pi->autostart_flag);
        /* A loader counts as installed when its folder (/data/etaHEN for
         * /data/etaHEN/payloads) exists; plat_info's loader_dir for the
         * running one. */
        p[i].present = (i == 0 && pi->loader_dir && !(env && *env)) ? dir_exists(pi->loader_dir) : parent_exists(dirs[i]);
    }
    return n;
}

/* Copies other tools keep, that exist. */
static int copies(char list[][PATH_MAX_LEN], int max) {
    const char *env = getenv("ROMM_SYNC_PAYLOAD_COPIES");
    int n = 0;
    if (env) {
        n = split(env, list, max);
    } else {
        const char *const *c = plat_info()->payload_copies;
        for (int i = 0; c && c[i] && n < max; i++) str_copy(list[n++], PATH_MAX_LEN, c[i]);
    }
    int kept = 0;
    for (int i = 0; i < n; i++)
        if (file_exists(list[i])) memmove(list[kept++], list[i], PATH_MAX_LEN);
    return kept;
}

static int enabled_at(const place *p) { return file_exists(p->elf) && (!p->flag[0] || file_exists(p->flag)); }

/* The version of the payload at path, "" if none or too old to say. Read
 * every time: two builds can have the same size and date to the second. */
static void file_version(const char *path, char *out, size_t n) {
    size_t len;
    char *data = read_file(path, &len);
    out[0] = 0;
    if (data) payload_version(data, len, out, n);
    free(data);
}

/* Writes the payload to path unless a newer one is already there. Same mode
 * "make install" leaves over FTP. 1 if written, 0 if kept, -1 on error. */
static int put(const char *path, const void *data, size_t len, const char *version) {
    char have[32];
    if (file_exists(path)) {
        file_version(path, have, sizeof have);
        if (version_cmp(have, version) > 0) {
            LOGI("kept %s: it's %s, newer than %s", path, have, version[0] ? version : "this payload");
            return 0;
        }
    }
    if (write_file_atomic(path, data, len) != 0) {
        LOGW("could not write %s", path);
        return -1;
    }
    chmod(path, 0777);
    return 1;
}

static void remove_bak(const place *p) {
    char bak[PATH_MAX_LEN + 8];
    snprintf(bak, sizeof bak, "%s.bak", p->elf);
    if (file_exists(bak) && unlink(bak) == 0) LOGI("removed %s", bak);
}

cJSON *autostart_status(void) {
    place p[MAX_PLACES];
    int n = places(p, MAX_PLACES);
    char version[32] = "";
    if (n && file_exists(p[0].elf)) file_version(p[0].elf, version, sizeof version);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "supported", n > 0);
    cJSON_AddStringToObject(j, "loader", plat_info()->loader);
    cJSON_AddStringToObject(j, "payload", plat_info()->payload);
    if (plat_info()->autostart_hint) cJSON_AddStringToObject(j, "hint", plat_info()->autostart_hint);
    cJSON_AddBoolToObject(j, "loader_found", n && p[0].present);
    cJSON_AddBoolToObject(j, "installed", n && file_exists(p[0].elf));
    cJSON_AddBoolToObject(j, "enabled", n && enabled_at(&p[0]));
    cJSON_AddStringToObject(j, "path", n ? p[0].elf : "");
    cJSON_AddStringToObject(j, "version", version);
    cJSON *list = cJSON_AddArrayToObject(j, "copies");
    char extra[MAX_PLACES][PATH_MAX_LEN];
    int ne = copies(extra, MAX_PLACES);
    for (int i = 0; i < n + ne; i++) {
        const char *path = i < n ? p[i].elf : extra[i - n];
        if (!file_exists(path)) continue;
        char v[32];
        file_version(path, v, sizeof v);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", path);
        cJSON_AddStringToObject(o, "version", v);
        cJSON_AddBoolToObject(o, "enabled", i < n ? enabled_at(&p[i]) : 1);
        cJSON_AddItemToArray(list, o);
    }
    return j;
}

/* Writes the payload to every installed loader's folder (or, with existing_only,
 * only over copies already there) and over other tools' copies. The number
 * written or kept, -1 if one couldn't be written. */
static int put_everywhere(const void *data, size_t len, int existing_only) {
    place p[MAX_PLACES];
    int n = places(p, MAX_PLACES), done = 0, failed = 0;
    char version[32];
    payload_version(data, len, version, sizeof version);
    for (int i = 0; i < n; i++) {
        if (!p[i].present || (existing_only && !file_exists(p[i].elf))) continue;
        int r = put(p[i].elf, data, len, version);
        if (r < 0) failed = 1;
        else done++;
        if (r > 0) LOGI("installed %s %s (%zu bytes)", version[0] ? version : "payload", p[i].elf, len);
        remove_bak(&p[i]);
    }
    char extra[MAX_PLACES][PATH_MAX_LEN];
    int ne = copies(extra, MAX_PLACES);
    for (int i = 0; i < ne; i++) {
        int r = put(extra[i], data, len, version);
        if (r < 0) failed = 1;
        else done++;
        if (r > 0) LOGI("updated %s to %s", extra[i], version[0] ? version : "this payload");
    }
    return failed ? -1 : done;
}

/* Turns autostart on in every installed loader's folder that has the payload. */
static int enable_all(char *err, int en) {
    place p[MAX_PLACES];
    int n = places(p, MAX_PLACES);
    for (int i = 0; i < n; i++) {
        if (!p[i].present || !p[i].flag[0] || !file_exists(p[i].elf) || file_exists(p[i].flag)) continue;
        if (write_file_atomic(p[i].flag, "", 0) != 0) {
            snprintf(err, (size_t)en, "cannot write %s", p[i].flag);
            return -1;
        }
    }
    return 0;
}

static int check_elf(const void *data, size_t len, char *err, int en) {
    if (len < 4 || memcmp(data, "\x7f" "ELF", 4) != 0) {
        snprintf(err, (size_t)en, "that file is not an ELF payload");
        return -1;
    }
    return 0;
}

int autostart_set(int enable, const void *data, size_t len, char *err, int en) {
    place p[MAX_PLACES];
    int n = places(p, MAX_PLACES);
    if (!n) {
        snprintf(err, (size_t)en, "%s can't start payloads with the console", plat_info()->loader);
        return -1;
    }
    if (!p[0].present) {
        snprintf(err, (size_t)en, "%s folder not found (%s)", plat_info()->loader, p[0].elf);
        return -1;
    }
    if (data && len) {
        if (check_elf(data, len, err, en)) return -1;
        if (put_everywhere(data, len, 0) < 0) {
            snprintf(err, (size_t)en, "cannot write %s", p[0].elf);
            return -1;
        }
    }
    if (enable) {
        if (!file_exists(p[0].elf)) {
            snprintf(err, (size_t)en, "upload %s first", plat_info()->payload);
            return -1;
        }
        /* A loader that's installed but has no copy yet gets the running
         * loader's, so the payload starts whichever one runs. */
        for (int i = 1; i < n; i++)
            if (p[i].present && !file_exists(p[i].elf) && copy_file(p[0].elf, p[i].elf) == 0) chmod(p[i].elf, 0777);
        if (enable_all(err, en)) return -1;
        LOGI("autostart enabled");
        return 0;
    }
    /* Off: nothing of ours is left in any loader's folder to start later. */
    for (int i = 0; i < n; i++) {
        if (p[i].flag[0]) unlink(p[i].flag);
        if (file_exists(p[i].elf) && unlink(p[i].elf) == 0) LOGI("removed %s", p[i].elf);
        remove_bak(&p[i]);
    }
    LOGI("autostart disabled");
    return 0;
}

int autostart_install(const void *data, size_t len, char *err, int en) {
    place p[MAX_PLACES];
    int n = places(p, MAX_PLACES);
    if (check_elf(data, len, err, en)) return -1;
    int on = 0;
    for (int i = 0; i < n; i++) on |= p[i].present && enabled_at(&p[i]);
    if (put_everywhere(data, len, !on) < 0) {
        snprintf(err, (size_t)en, "cannot write the payload to %s's folder", plat_info()->loader);
        return -1;
    }
    return on ? enable_all(err, en) : 0;
}

int autostart_replace(const void *data, size_t len, char *err, int en) {
    int r = put_everywhere(data, len, 1);
    if (r < 0) {
        snprintf(err, (size_t)en, "cannot write the payload to %s's folder", plat_info()->loader);
        return -1;
    }
    return r > 0;
}

void autostart_repair(void) {
    place p[MAX_PLACES];
    int n = places(p, MAX_PLACES);
    char extra[MAX_PLACES][PATH_MAX_LEN];
    int ne = copies(extra, MAX_PLACES);

    /* The newest copy on the console, and whether autostart is on anywhere. */
    char best[PATH_MAX_LEN] = "", best_v[32] = "";
    int on = 0;
    for (int i = 0; i < n + ne; i++) {
        const char *path = i < n ? p[i].elf : extra[i - n];
        if (i < n) {
            remove_bak(&p[i]);
            if (!p[i].present) continue;
            on |= enabled_at(&p[i]);
        }
        if (!file_exists(path)) continue;
        char v[32];
        file_version(path, v, sizeof v);
        if (!best[0] || version_cmp(v, best_v) > 0) {
            str_copy(best, sizeof best, path);
            str_copy(best_v, sizeof best_v, v);
        }
    }
    if (!best[0]) return;

    for (int i = 0; i < n + ne; i++) {
        const char *path = i < n ? p[i].elf : extra[i - n];
        if (i < n && !p[i].present) continue;
        int have = file_exists(path);
        /* Missing copies are only added to loaders, and only while it's on. */
        if (!have && !(i < n && on)) continue;
        if (have) {
            char v[32];
            file_version(path, v, sizeof v);
            if (version_cmp(v, best_v) >= 0) continue;
        }
        if (copy_file(best, path) == 0) {
            chmod(path, 0777);
            LOGI("%s %s with %s from %s", have ? "updated" : "set up", path, best_v[0] ? best_v : "the payload", best);
        } else {
            LOGW("could not copy %s to %s", best, path);
        }
    }
    if (on) {
        char err[256];
        if (enable_all(err, sizeof err)) LOGW("%s", err);
    }
}
