#include "memcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "platform.h"
#include "profiles.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "util.h"
#include "zip.h"

#define EMULATOR  "lrps2"
#define MAX_SLOTS 2

/* LRPS2 keeps shared cards in <system>/pcsx2/memcards/Mcd00N.ps2. RomM only
 * takes cards as zips, so each one goes up as a zip holding exactly one file. */
static int cards_dir(char *out, size_t n) {
    profile_map *maps = NULL;
    config_lock();
    int cnt = profiles_expand(g_cfg.profiles, &maps);
    config_unlock();
    int found = 0;
    for (int i = 0; i < cnt && !found; i++) {
        if (profile_map_handles(&maps[i], "ps2", NULL) && str_ieq(maps[i].core_name, "LRPS2")) {
            snprintf(out, n, "%s/pcsx2/memcards", maps[i].bios_dir);
            found = 1;
        }
    }
    free(maps);
    return found;
}

static void slot_path(const char *dir, int slot, char *out, size_t n) { snprintf(out, n, "%s/Mcd%03d.ps2", dir, slot); }

int memcard_enabled(void) {
    char mode[16], dir[PATH_MAX_LEN];
    config_lock();
    str_copy(mode, sizeof mode, g_cfg.ps2_cards);
    config_unlock();
    return !strcmp(mode, "backup") && config_server_at_least(5, 3, 0) && cards_dir(dir, sizeof dir);
}

int memcard_dirs(char dirs[][1024], int max) {
    if (max < 1 || !memcard_enabled()) return 0;
    return cards_dir(dirs[0], 1024) ? 1 : 0;
}

static int find_or_create_card(int slot) {
    char name[128], device[64];
    config_lock();
    str_copy(device, sizeof device, g_cfg.device_name);
    config_unlock();
    snprintf(name, sizeof name, "%s slot %d", device, slot);

    long st = 0;
    cJSON *list = romm_call("GET", "/api/memory-cards?emulator=" EMULATOR, NULL, &st);
    int id = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, list) if (!strcmp(jget_str(c, "name", ""), name)) id = (int)jget_num(c, "id", 0);
    cJSON_Delete(list);
    if (id) return id;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    cJSON_AddStringToObject(body, "emulator", EMULATOR);
    cJSON *card = romm_call("POST", "/api/memory-cards", body, &st);
    cJSON_Delete(body);
    id = (st == 200 || st == 201) ? (int)jget_num(card, "id", 0) : 0;
    cJSON_Delete(card);
    if (id) LOGI("created memory card \"%s\" on RomM", name);
    return id;
}

static int upload_slot(const char *path, int slot, const char *hash) {
    state_lock();
    cJSON *e = state_entry_ensure("memcards", path);
    int card_id = (int)jget_num(e, "card_id", 0);
    state_unlock();

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!card_id) card_id = find_or_create_card(slot);
        if (!card_id) {
            LOGE("could not create a memory card on RomM");
            return -1;
        }
        char zip[PATH_MAX_LEN], entry[PATH_MAX_LEN], api[128];
        snprintf(zip, sizeof zip, "%s/tmp/card-%d.zip", plat_data_dir(), slot);
        mkdir_parent(zip);
        str_copy(entry, sizeof entry, path);
        const char *files[] = {entry};
        if (zip_store_files(zip, files, 1) != 0) {
            LOGE("could not pack %s", path);
            return -1;
        }
        snprintf(api, sizeof api, "/api/memory-cards/%d/versions", card_id);
        long st = 0;
        cJSON *v = romm_upload("POST", api, "cardFile", zip, path_basename(zip), &st);
        unlink(zip);
        if (st == 200 || st == 201) {
            state_lock();
            cJSON *s = state_entry_ensure("memcards", path);
            jset_num(s, "card_id", card_id);
            jset_str(s, "hash", hash);
            jset_num(s, "version_id", jget_num(v, "id", 0));
            jset_num(s, "backed_up_at", (double)time(NULL));
            state_save();
            state_unlock();
            cJSON_Delete(v);
            LOGI("backed up memory card %s", path_basename(path));
            return 0;
        }
        cJSON_Delete(v);
        if (st != 404) return -1;
        card_id = 0; /* deleted on the server, create it again */
    }
    return -1;
}

int memcard_backup(void) {
    char dir[PATH_MAX_LEN];
    if (!memcard_enabled() || !cards_dir(dir, sizeof dir)) return 0;
    int uploaded = 0;
    for (int slot = 1; slot <= MAX_SLOTS; slot++) {
        char path[PATH_MAX_LEN], hash[33];
        slot_path(dir, slot, path, sizeof path);
        if (!file_exists(path) || md5_file_hex(path, hash) != 0) continue;
        state_lock();
        cJSON *e = state_entry("memcards", path);
        int same = e && !strcmp(jget_str(e, "hash", ""), hash);
        state_unlock();
        if (!same && upload_slot(path, slot, hash) == 0) uploaded++;
    }
    return uploaded;
}

int memcard_restore(int slot, char *err, int en) {
    char dir[PATH_MAX_LEN], path[PATH_MAX_LEN];
    if (!memcard_enabled() || !cards_dir(dir, sizeof dir)) {
        snprintf(err, (size_t)en, "memory card backup is not turned on");
        return -1;
    }
    if (slot < 1 || slot > MAX_SLOTS) {
        snprintf(err, (size_t)en, "no such slot");
        return -1;
    }
    if (sync_game_running()) {
        snprintf(err, (size_t)en, "close RetroArch before restoring a memory card");
        return -1;
    }
    slot_path(dir, slot, path, sizeof path);
    state_lock();
    cJSON *e = state_entry("memcards", path);
    int card_id = e ? (int)jget_num(e, "card_id", 0) : 0;
    state_unlock();
    if (!card_id) card_id = find_or_create_card(slot);

    char api[128], zip[PATH_MAX_LEN], tmp[PATH_MAX_LEN];
    snprintf(api, sizeof api, "/api/memory-cards/%d/content", card_id);
    snprintf(zip, sizeof zip, "%s/tmp/restore-%d.zip", plat_data_dir(), slot);
    snprintf(tmp, sizeof tmp, "%s/tmp/restore-%d.ps2", plat_data_dir(), slot);
    http_resp r;
    if (romm_download(api, zip, NULL, NULL, &r) != 0) {
        snprintf(err, (size_t)en, r.status == 404 ? "no backup on RomM yet" : "download failed");
        unlink(zip);
        return -1;
    }
    int rc = zip_extract_entry(zip, path_basename(path), tmp);
    unlink(zip);
    if (rc != 0) {
        snprintf(err, (size_t)en, "the backup on RomM is not an LRPS2 card");
        return -1;
    }
    if (file_exists(path)) {
        char bak[PATH_MAX_LEN], ts[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
        snprintf(bak, sizeof bak, "%s/backups/memcards/%s.%s", plat_data_dir(), path_basename(path), ts);
        copy_file(path, bak);
    }
    mkdir_parent(path);
    if (move_file(tmp, path) != 0) {
        snprintf(err, (size_t)en, "could not write %s", path);
        return -1;
    }
    char hash[33];
    md5_file_hex(path, hash);
    state_lock();
    cJSON *s = state_entry_ensure("memcards", path);
    jset_num(s, "card_id", card_id);
    jset_str(s, "hash", hash);
    state_save();
    state_unlock();
    LOGI("restored memory card %s from RomM", path_basename(path));
    return 0;
}

cJSON *memcard_status(void) {
    char dir[PATH_MAX_LEN], mode[16], version[32];
    config_lock();
    str_copy(mode, sizeof mode, g_cfg.ps2_cards);
    str_copy(version, sizeof version, g_cfg.server_version);
    config_unlock();
    int have_dir = cards_dir(dir, sizeof dir);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", mode);
    cJSON_AddStringToObject(j, "server_version", version);
    cJSON_AddBoolToObject(j, "supported", config_server_at_least(5, 3, 0));
    cJSON_AddBoolToObject(j, "lrps2", have_dir);
    cJSON_AddBoolToObject(j, "enabled", memcard_enabled());
    cJSON *slots = cJSON_AddArrayToObject(j, "slots");
    for (int slot = 1; have_dir && slot <= MAX_SLOTS; slot++) {
        char path[PATH_MAX_LEN];
        slot_path(dir, slot, path, sizeof path);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", slot);
        cJSON_AddStringToObject(o, "path", path);
        cJSON_AddBoolToObject(o, "exists", file_exists(path));
        state_lock();
        cJSON *e = state_entry("memcards", path);
        cJSON_AddNumberToObject(o, "backed_up_at", e ? jget_num(e, "backed_up_at", 0) : 0);
        state_unlock();
        cJSON_AddItemToArray(slots, o);
    }
    return j;
}
