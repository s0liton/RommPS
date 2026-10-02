#include "autostart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

/* etaHEN starts each payloads/<name>.elf that has a <name>.elf.auto_start next to it. */
static const char *etahen_dir(void) {
    const char *e = getenv("ROMM_SYNC_ETAHEN");
    return e && *e ? e : "/data/etaHEN";
}

static void paths(char *elf, char *flag, size_t n) {
    snprintf(elf, n, "%s/payloads/romm-sync.elf", etahen_dir());
    snprintf(flag, n, "%s/payloads/romm-sync.elf.auto_start", etahen_dir());
}

cJSON *autostart_status(void) {
    char elf[PATH_MAX_LEN], flag[PATH_MAX_LEN];
    paths(elf, flag, sizeof elf);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "etahen", dir_exists(etahen_dir()));
    cJSON_AddBoolToObject(j, "installed", file_exists(elf));
    cJSON_AddBoolToObject(j, "enabled", file_exists(elf) && file_exists(flag));
    cJSON_AddStringToObject(j, "path", elf);
    return j;
}

int autostart_set(int enable, const void *data, size_t len, char *err, int en) {
    char elf[PATH_MAX_LEN], flag[PATH_MAX_LEN];
    paths(elf, flag, sizeof elf);
    if (!dir_exists(etahen_dir())) {
        snprintf(err, (size_t)en, "etaHEN folder not found (%s)", etahen_dir());
        return -1;
    }
    if (data && len) {
        /* Only accept ELF files. */
        if (len < 4 || memcmp(data, "\x7f" "ELF", 4) != 0) {
            snprintf(err, (size_t)en, "that file is not an ELF payload");
            return -1;
        }
        if (write_file_atomic(elf, data, len) != 0) {
            snprintf(err, (size_t)en, "cannot write %s", elf);
            return -1;
        }
        LOGI("installed payload to %s (%zu bytes)", elf, len);
    }
    if (enable) {
        if (!file_exists(elf)) {
            snprintf(err, (size_t)en, "upload romm-sync.elf first");
            return -1;
        }
        if (write_file_atomic(flag, "", 0) != 0) {
            snprintf(err, (size_t)en, "cannot write %s", flag);
            return -1;
        }
        LOGI("autostart enabled");
    } else {
        unlink(flag);
        LOGI("autostart disabled");
    }
    return 0;
}
