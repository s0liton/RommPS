#include "memcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "platform.h"
#include "profiles.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "util.h"
#include "zip.h"

/* The shared memory cards backed up: LRPS2's (<system>/pcsx2/memcards/
 * Mcd00N.ps2, when its "backup" mode is on) and those a profile lists in
 * "memcards" (PS5SX2's Mcd001.ps2 and Mcd002.ps2). RomM only takes cards as
 * zips, so each goes up as a zip holding exactly one file. */
#define MAX_CARDS 8

typedef struct {
    char path[PATH_MAX_LEN];
    char emulator[32]; /* RomM's emulator field: "lrps2", or the profile's id */
    char label[64];    /* in the card's name on RomM; "" for LRPS2's */
    int slot;          /* 1-based, per emulator */
} card;

static int cards_list(card *out, int max) {
    char mode[16];
    config_lock();
    str_copy(mode, sizeof mode, g_cfg.ps2_cards);
    profile_map *maps = NULL;
    int cnt = profiles_expand(g_cfg.profiles, &maps);
    config_unlock();
    int n = 0;
    for (int i = 0; i < cnt && n < max; i++) {
        const profile_map *m = &maps[i];
        if (!strcmp(mode, "backup") && profile_map_handles(m, "ps2", NULL) && str_ieq(m->core_name, "LRPS2")) {
            for (int s = 1; s <= 2 && n < max; s++) {
                card *c = &out[n++];
                snprintf(c->path, sizeof c->path, "%s/pcsx2/memcards/Mcd%03d.ps2", m->bios_dir, s);
                str_copy(c->emulator, sizeof c->emulator, "lrps2");
                c->label[0] = 0;
                c->slot = s;
            }
        }
        char list[512], *save = NULL;
        str_copy(list, sizeof list, m->memcards);
        int s = 0;
        for (char *t = strtok_r(list, ",", &save); t && n < max; t = strtok_r(NULL, ",", &save)) {
            s++;
            int dup = 0;
            for (int k = 0; k < n; k++) dup |= !strcmp(out[k].path, t);
            if (dup) continue;
            card *c = &out[n++];
            str_copy(c->path, sizeof c->path, t);
            str_copy(c->emulator, sizeof c->emulator, m->profile_id);
            str_copy(c->label, sizeof c->label, m->profile_name);
            c->slot = s;
        }
    }
    free(maps);
    return n;
}

int memcard_enabled(void) {
    card cards[MAX_CARDS];
    return config_server_at_least(5, 3, 0) && cards_list(cards, MAX_CARDS) > 0;
}

int memcard_dirs(char dirs[][1024], int max) {
    if (max < 1 || !memcard_enabled()) return 0;
    card cards[MAX_CARDS];
    int n = cards_list(cards, MAX_CARDS), nd = 0;
    for (int i = 0; i < n && nd < max; i++) {
        char dir[1024];
        str_copy(dir, sizeof dir, cards[i].path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        int dup = 0;
        for (int k = 0; k < nd; k++) dup |= !strcmp(dirs[k], dir);
        if (!dup) str_copy(dirs[nd++], 1024, dir);
    }
    return nd;
}

static int find_or_create_card(const card *cd) {
    char name[160], device[64], api[128];
    config_lock();
    str_copy(device, sizeof device, g_cfg.device_name);
    config_unlock();
    if (cd->label[0]) snprintf(name, sizeof name, "%s %s slot %d", device, cd->label, cd->slot);
    else snprintf(name, sizeof name, "%s slot %d", device, cd->slot);

    long st = 0;
    char *q = http_escape(cd->emulator);
    snprintf(api, sizeof api, "/api/memory-cards?emulator=%s", q);
    free(q);
    cJSON *list = romm_call("GET", api, NULL, &st);
    int id = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, list) if (!strcmp(jget_str(c, "name", ""), name)) id = (int)jget_num(c, "id", 0);
    cJSON_Delete(list);
    if (id) return id;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    cJSON_AddStringToObject(body, "emulator", cd->emulator);
    cJSON *made = romm_call("POST", "/api/memory-cards", body, &st);
    cJSON_Delete(body);
    id = (st == 200 || st == 201) ? (int)jget_num(made, "id", 0) : 0;
    cJSON_Delete(made);
    if (id) LOGI("created memory card \"%s\" on RomM", name);
    return id;
}

static int upload_slot(const card *cd, const char *hash) {
    const char *path = cd->path;
    state_lock();
    cJSON *e = state_entry_ensure("memcards", path);
    int card_id = (int)jget_num(e, "card_id", 0);
    state_unlock();

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!card_id) card_id = find_or_create_card(cd);
        if (!card_id) {
            LOGE("could not create a memory card on RomM");
            return -1;
        }
        char zip[PATH_MAX_LEN], entry[PATH_MAX_LEN], api[128];
        snprintf(zip, sizeof zip, "%s/tmp/card-%s-%d.zip", plat_data_dir(), cd->emulator, cd->slot);
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
    if (!memcard_enabled()) return 0;
    card cards[MAX_CARDS];
    int n = cards_list(cards, MAX_CARDS), uploaded = 0;
    for (int i = 0; i < n; i++) {
        char hash[33];
        if (!file_exists(cards[i].path) || md5_file_hex(cards[i].path, hash) != 0) continue;
        state_lock();
        cJSON *e = state_entry("memcards", cards[i].path);
        int same = e && !strcmp(jget_str(e, "hash", ""), hash);
        state_unlock();
        if (!same && upload_slot(&cards[i], hash) == 0) uploaded++;
    }
    return uploaded;
}

int memcard_restore(int slot, char *err, int en) {
    card cards[MAX_CARDS];
    int n = memcard_enabled() ? cards_list(cards, MAX_CARDS) : 0;
    if (!n) {
        snprintf(err, (size_t)en, "memory card backup is not turned on");
        return -1;
    }
    if (slot < 1 || slot > n) {
        snprintf(err, (size_t)en, "no such slot");
        return -1;
    }
    const card *cd = &cards[slot - 1];
    char path[PATH_MAX_LEN];
    if (sync_game_running()) {
        snprintf(err, (size_t)en, "close the emulator before restoring a memory card");
        return -1;
    }
    str_copy(path, sizeof path, cd->path);
    state_lock();
    cJSON *e = state_entry("memcards", path);
    int card_id = e ? (int)jget_num(e, "card_id", 0) : 0;
    state_unlock();
    if (!card_id) card_id = find_or_create_card(cd);

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
        snprintf(err, (size_t)en, "the backup on RomM isn't a card for this slot");
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
    char mode[16], version[32];
    config_lock();
    str_copy(mode, sizeof mode, g_cfg.ps2_cards);
    str_copy(version, sizeof version, g_cfg.server_version);
    config_unlock();
    card cards[MAX_CARDS];
    int n = cards_list(cards, MAX_CARDS), lrps2 = 0;
    for (int i = 0; i < n; i++) lrps2 |= !strcmp(cards[i].emulator, "lrps2");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", mode);
    cJSON_AddStringToObject(j, "server_version", version);
    cJSON_AddBoolToObject(j, "supported", config_server_at_least(5, 3, 0));
    cJSON_AddBoolToObject(j, "lrps2", lrps2);
    cJSON_AddBoolToObject(j, "enabled", memcard_enabled());
    cJSON *slots = cJSON_AddArrayToObject(j, "slots");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i + 1);
        cJSON_AddStringToObject(o, "emulator", cards[i].label[0] ? cards[i].label : "LRPS2");
        cJSON_AddNumberToObject(o, "card", cards[i].slot);
        cJSON_AddStringToObject(o, "path", cards[i].path);
        cJSON_AddBoolToObject(o, "exists", file_exists(cards[i].path));
        state_lock();
        cJSON *e = state_entry("memcards", cards[i].path);
        cJSON_AddNumberToObject(o, "backed_up_at", e ? jget_num(e, "backed_up_at", 0) : 0);
        state_unlock();
        cJSON_AddItemToArray(slots, o);
    }
    return j;
}
