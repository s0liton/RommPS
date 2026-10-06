#include "autostart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"
#include "util.h"

/* The folder the loader starts payloads from (plat_info), or NULL if the
 * loader can't start payloads at boot. */
static const char *autostart_dir(void) {
    const char *e = getenv("ROMM_SYNC_AUTOSTART");
    return e && *e ? e : plat_info()->autostart_dir;
}

/* The loader counts as installed when plat_info's loader_dir exists, or else the
 * folder above its payload folder: /data/etaHEN for /data/etaHEN/payloads. */
static int loader_found(void) {
    char parent[PATH_MAX_LEN];
    if (!autostart_dir()) return 0;
    if (plat_info()->loader_dir && !getenv("ROMM_SYNC_AUTOSTART")) return dir_exists(plat_info()->loader_dir);
    str_copy(parent, sizeof parent, autostart_dir());
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0;
    return dir_exists(parent);
}

/* Same mode "make install" leaves over FTP. */
static int write_payload(const char *elf, const void *data, size_t len) {
    if (write_file_atomic(elf, data, len) != 0) return -1;
    chmod(elf, 0777);
    return 0;
}

/* flag is "" when the payload's presence is enough. Both are "" without autostart. */
static void paths(char *elf, char *flag, size_t n) {
    const plat_info_t *pi = plat_info();
    elf[0] = flag[0] = 0;
    if (!autostart_dir()) return;
    snprintf(elf, n, "%s/%s", autostart_dir(), pi->payload);
    if (pi->autostart_flag) snprintf(flag, n, "%s/%s", autostart_dir(), pi->autostart_flag);
}

cJSON *autostart_status(void) {
    char elf[PATH_MAX_LEN], flag[PATH_MAX_LEN];
    paths(elf, flag, sizeof elf);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "supported", autostart_dir() != NULL);
    cJSON_AddStringToObject(j, "loader", plat_info()->loader);
    cJSON_AddStringToObject(j, "payload", plat_info()->payload);
    if (plat_info()->autostart_hint) cJSON_AddStringToObject(j, "hint", plat_info()->autostart_hint);
    cJSON_AddBoolToObject(j, "loader_found", loader_found());
    cJSON_AddBoolToObject(j, "installed", elf[0] && file_exists(elf));
    cJSON_AddBoolToObject(j, "enabled", elf[0] && file_exists(elf) && (!flag[0] || file_exists(flag)));
    cJSON_AddStringToObject(j, "path", elf);
    return j;
}

int autostart_replace(const void *data, size_t len, char *err, int en) {
    char elf[PATH_MAX_LEN], flag[PATH_MAX_LEN], bak[PATH_MAX_LEN + 8];
    paths(elf, flag, sizeof elf);
    if (!elf[0] || !file_exists(elf)) return 0;
    snprintf(bak, sizeof bak, "%s.bak", elf);
    if (copy_file(elf, bak) != 0) {
        snprintf(err, (size_t)en, "cannot back up %s", elf);
        return -1;
    }
    if (write_payload(elf, data, len) != 0) {
        snprintf(err, (size_t)en, "cannot write %s", elf);
        return -1;
    }
    LOGI("replaced %s (%zu bytes), previous version kept as %s", elf, len, bak);
    return 1;
}

int autostart_set(int enable, const void *data, size_t len, char *err, int en) {
    char elf[PATH_MAX_LEN], flag[PATH_MAX_LEN];
    paths(elf, flag, sizeof elf);
    if (!autostart_dir()) {
        snprintf(err, (size_t)en, "%s can't start payloads with the console", plat_info()->loader);
        return -1;
    }
    if (!loader_found()) {
        snprintf(err, (size_t)en, "%s folder not found (%s)", plat_info()->loader, autostart_dir());
        return -1;
    }
    if (data && len) {
        /* Only accept ELF files. */
        if (len < 4 || memcmp(data, "\x7f" "ELF", 4) != 0) {
            snprintf(err, (size_t)en, "that file is not an ELF payload");
            return -1;
        }
        if (write_payload(elf, data, len) != 0) {
            snprintf(err, (size_t)en, "cannot write %s", elf);
            return -1;
        }
        LOGI("installed payload to %s (%zu bytes)", elf, len);
    }
    if (enable) {
        if (!file_exists(elf)) {
            snprintf(err, (size_t)en, "upload %s first", plat_info()->payload);
            return -1;
        }
        if (flag[0] && write_file_atomic(flag, "", 0) != 0) {
            snprintf(err, (size_t)en, "cannot write %s", flag);
            return -1;
        }
        LOGI("autostart enabled");
    } else if (flag[0]) {
        unlink(flag);
        LOGI("autostart disabled");
    } else {
        /* Nothing but the payload itself turns autostart on, so remove it. */
        unlink(elf);
        LOGI("autostart disabled, removed %s", elf);
    }
    return 0;
}
