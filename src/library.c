#include "library.h"

#include <pthread.h>
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
#include "util.h"

#define MAX_WORKERS 8
#define MAX_JOBS    50

typedef enum { JOB_ROM, JOB_FIRMWARE } job_type;
typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_ERROR, JOB_CANCELLED } job_status;

typedef struct job {
    int id;
    job_type type;
    int target_id; /* rom id or platform id */
    char name[256];
    char platform[64];
    job_status status;
    int cancel;
    char error[256];
    int64_t done, total;
    int64_t base; /* bytes from files already finished (multi-file games) */
    double speed; /* bytes/s, smoothed */
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

static void job_end(job *j, job_status st, const char *fmt, const char *arg) {
    pthread_mutex_lock(&g_q_lock);
    j->status = j->cancel ? JOB_CANCELLED : st;
    if (fmt && j->status == JOB_ERROR) snprintf(j->error, sizeof j->error, fmt, arg ? arg : "");
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
    state_save();
    state_unlock();
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
    str_copy(j->platform, sizeof j->platform, jget_str(rom, "platform_display_name", jget_str(rom, "platform_slug", "")));
    j->total = (int64_t)jget_num(rom, "fs_size_bytes", 0);
    pthread_mutex_unlock(&g_q_lock);

    profile_map *maps = NULL;
    int n = load_maps(&maps);
    const char *slug = jget_str(rom, "platform_slug", "");
    const profile_map *m = profile_find_for_platform(maps, n, slug, jget_str(rom, "platform_fs_slug", ""));
    if (!m) {
        job_end(j, JOB_ERROR, "no emulator is set up for %s", slug);
        free(maps);
        cJSON_Delete(rom);
        return;
    }

    const char *fs_name = jget_str(rom, "fs_name", "");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files");
    int multi = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rom, "has_multiple_files")) &&
                cJSON_GetArraySize(files) > 1;
    path_join(dest, sizeof dest, m->rom_dir, fs_name);
    mkdir_p(m->rom_dir);
    int ok = 1;
    http_resp r;
    if (!multi) {
        char *q = http_escape(fs_name);
        snprintf(path, sizeof path, "/api/roms/%d/content/%s", j->target_id, q);
        free(q);
        ok = romm_download(path, dest, progress_cb, j, &r) == 0;
    } else {
        /* Fetched file by file into <rom_dir>/<fs_name>/ so nothing needs unzipping. */
        int64_t base = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const char *fn = jget_str(f, "file_name", "");
            char fdest[PATH_MAX_LEN];
            path_join(fdest, sizeof fdest, dest, fn);
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
        record_rom(j, rom, m, dest, multi);
        LOGI("downloaded %s to %s", j->name, dest);
        /* The extra emulators for this system get the same game: a hard link
         * where they share a drive, a copy where they don't. */
        for (int i = 0; i < n; i++) {
            const profile_map *x = &maps[i];
            if (x == m || !x->extra || !profile_map_handles(x, slug, jget_str(rom, "platform_fs_slug", ""))) continue;
            char other[PATH_MAX_LEN];
            path_join(other, sizeof other, x->rom_dir, fs_name);
            if (!strcmp(other, dest)) continue;
            mkdir_p(x->rom_dir);
            if (link_or_copy(dest, other) == 0) {
                record_rom(j, rom, x, other, multi);
                LOGI("also for %s: %s", x->profile_name, other);
            } else {
                LOGW("could not put %s in %s", j->name, x->rom_dir);
            }
        }
        job_end(j, JOB_DONE, NULL, NULL);
    } else {
        if (cancelled(j)) discard_partial(dest, multi);
        job_end(j, JOB_ERROR, "%s", r.error);
    }
    free(maps);
    cJSON_Delete(rom);
}

static void run_firmware_job(job *j) {
    char path[1024];
    profile_map *maps = NULL;
    int n = load_maps(&maps);

    long st = 0;
    snprintf(path, sizeof path, "/api/platforms/%d", j->target_id);
    cJSON *plat = romm_call("GET", path, NULL, &st);
    pthread_mutex_lock(&g_q_lock);
    str_copy(j->platform, sizeof j->platform, jget_str(plat, "display_name", jget_str(plat, "name", "")));
    pthread_mutex_unlock(&g_q_lock);
    const profile_map *m =
        plat ? profile_find_for_platform(maps, n, jget_str(plat, "slug", ""), jget_str(plat, "fs_slug", "")) : NULL;
    if (!m) {
        job_end(j, JOB_ERROR, "no emulator is set up for %s", plat ? jget_str(plat, "slug", "") : "this platform");
        cJSON_Delete(plat);
        free(maps);
        return;
    }
    snprintf(path, sizeof path, "/api/firmware?platform_id=%d", j->target_id);
    cJSON *fw = romm_call("GET", path, NULL, &st);
    int count = 0, failed = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, fw) {
        if (cancelled(j)) break;
        const char *fn = jget_str(f, "file_name", "");
        char dest[PATH_MAX_LEN];
        path_join(dest, sizeof dest, m->bios_dir, fn);
        if (file_size(dest) == (int64_t)jget_num(f, "file_size_bytes", -1)) continue;
        char *q = http_escape(fn);
        snprintf(path, sizeof path, "/api/firmware/%d/content/%s", (int)jget_num(f, "id", 0), q);
        free(q);
        http_resp r;
        if (romm_download(path, dest, progress_cb, j, &r) == 0) {
            count++;
            LOGI("downloaded BIOS %s", fn);
        } else {
            discard_partial(dest, 0);
            failed++;
        }
    }
    pthread_mutex_lock(&g_q_lock);
    snprintf(j->name, sizeof j->name, "BIOS files (%d new)", count);
    pthread_mutex_unlock(&g_q_lock);
    char msg[32];
    snprintf(msg, sizeof msg, "%d file(s) failed", failed);
    job_end(j, failed ? JOB_ERROR : JOB_DONE, "%s", msg);
    cJSON_Delete(fw);
    cJSON_Delete(plat);
    free(maps);
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

void library_init(void) {
    for (int i = 0; i < MAX_WORKERS; i++) {
        thread_start(worker, NULL);
    }
}

int library_busy(void) {
    int busy = 0;
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j && !busy; j = j->next) busy = j->status == JOB_QUEUED || j->status == JOB_RUNNING;
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

static int enqueue(job_type type, int id, const char *name, char *err, int en) {
    pthread_mutex_lock(&g_q_lock);
    for (job *p = g_jobs; p; p = p->next) {
        if (p->type == type && p->target_id == id && (p->status == JOB_QUEUED || p->status == JOB_RUNNING)) {
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
    str_copy(j->name, sizeof j->name, name);
    j->id = ++g_job_seq;
    j->next = g_jobs;
    g_jobs = j;
    trim_jobs();
    pthread_cond_broadcast(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
    return 0;
}

int library_queue_rom(int rom_id, char *err, int en) {
    char name[32];
    snprintf(name, sizeof name, "Game #%d", rom_id);
    return enqueue(JOB_ROM, rom_id, name, err, en);
}

int library_queue_firmware(int platform_id, char *err, int en) {
    return enqueue(JOB_FIRMWARE, platform_id, "BIOS files", err, en);
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
        } else {
            snprintf(err, (size_t)en, "download already finished");
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

int library_delete_rom(int rom_id, char *err, int en) {
    int removed = 0;
    state_lock();
    cJSON *roms = state_section("roms"), *e = roms ? roms->child : NULL;
    while (e) {
        cJSON *next = e->next;
        if ((int)jget_num(e, "rom_id", -1) == rom_id) {
            LOGI("removing %s (saves are kept)", e->string);
            remove_tree(e->string);
            cJSON_DeleteItemFromObjectCaseSensitive(roms, e->string);
            removed++;
        }
        e = next;
    }
    state_save();
    state_unlock();
    if (!removed) snprintf(err, (size_t)en, "game is not on this console");
    return removed ? 0 : -1;
}

cJSON *library_downloads(void) {
    static const char *names[] = {"queued", "downloading", "done", "error", "cancelled"};
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j; j = j->next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", j->id);
        cJSON_AddStringToObject(o, "type", j->type == JOB_ROM ? "rom" : "firmware");
        cJSON_AddNumberToObject(o, j->type == JOB_ROM ? "rom_id" : "platform_id", j->target_id);
        cJSON_AddStringToObject(o, "name", j->name);
        cJSON_AddStringToObject(o, "platform", j->platform);
        cJSON_AddStringToObject(o, "status", j->status == JOB_RUNNING && j->cancel ? "cancelling" : names[j->status]);
        cJSON_AddStringToObject(o, "error", j->error);
        cJSON_AddNumberToObject(o, "done", (double)j->done);
        cJSON_AddNumberToObject(o, "total", (double)j->total);
        cJSON_AddNumberToObject(o, "speed", j->status == JOB_RUNNING ? j->speed : 0);
        cJSON_AddNumberToObject(o, j->type == JOB_ROM ? "rom_id" : "platform_id", j->target_id);
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&g_q_lock);
    return arr;
}

cJSON *library_platforms(char *err, int en) {
    long st = 0;
    cJSON *plats = romm_call("GET", "/api/platforms", NULL, &st);
    if (st != 200 || !cJSON_IsArray(plats)) {
        snprintf(err, (size_t)en, "could not load platforms (HTTP %ld)", st);
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
        cJSON_AddStringToObject(o, "name", jget_str(p, "display_name", jget_str(p, "name", "")));
        cJSON_AddNumberToObject(o, "rom_count", jget_num(p, "rom_count", 0));
        cJSON_AddNumberToObject(o, "firmware_count", jget_num(p, "firmware_count", 0));
        if (m) {
            cJSON_AddStringToObject(o, "profile", m->profile_name);
            cJSON_AddStringToObject(o, "rom_dir", m->rom_dir);
        }
        cJSON_AddItemToArray(out, o);
    }
    free(maps);
    cJSON_Delete(plats);
    return out;
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
        snprintf(err, (size_t)en, "could not load the game from RomM (HTTP %ld)", st);
        cJSON_Delete(rom);
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", rom_id);
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

cJSON *library_installed(int limit) {
    backfill_installed();
    cJSON *out = cJSON_CreateArray();
    const cJSON *e, *sv;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        int id = (int)jget_num(e, "rom_id", 0);
        if (id <= 0 || !(file_exists(e->string) || dir_exists(e->string))) continue;
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
        cJSON_AddItemToArray(out, o);
    }
    state_unlock();
    /* Newest first (a handful of games: a simple selection sort will do). */
    int n = cJSON_GetArraySize(out);
    cJSON *sorted = cJSON_CreateArray();
    for (int k = 0; k < n && k < limit; k++) {
        int best = -1;
        double top = -1;
        for (int i = 0; i < cJSON_GetArraySize(out); i++) {
            double t = jget_num(cJSON_GetArrayItem(out, i), "played", 0);
            if (t > top) top = t, best = i;
        }
        cJSON_AddItemToArray(sorted, cJSON_DetachItemFromArray(out, best));
    }
    cJSON_Delete(out);
    return sorted;
}

/* Whether a game is downloaded: a local file or folder the state maps to it. */
static int rom_installed(int id) {
    const cJSON *e;
    int installed = 0;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        if ((int)jget_num(e, "rom_id", -1) == id && (file_exists(e->string) || dir_exists(e->string))) {
            installed = 1;
            break;
        }
    }
    state_unlock();
    return installed;
}

/* Recent library pages, so going back, changing letters or reopening the UI
 * doesn't fetch the same games again over a slow link. Installed flags are
 * worked out again on every hit. */
#define PAGE_CACHE_SLOTS 32
#define PAGE_CACHE_SEC   300
static struct {
    char key[600];
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
    cJSON_AddNumberToObject(o, "platform_id", jget_num(it, "platform_id", 0));
    cJSON_AddBoolToObject(o, "multi", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "has_multiple_files")));
    cJSON_AddBoolToObject(o, "installed", rom_installed(id));
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(out, "items"), o);
    return 0;
}

cJSON *library_roms(int platform_id, const char *search, int offset, int limit, char *err, int en) {
    char path[1024];
    char *q = http_escape(search ? search : "");
    /* Platform 0: every platform (a search across the library). */
    if (platform_id > 0)
        snprintf(path, sizeof path, "/api/roms?platform_ids=%d&search_term=%s&order_by=name&order_dir=asc", platform_id, q);
    else
        snprintf(path, sizeof path, "/api/roms?search_term=%s&order_by=name&order_dir=asc", q);
    free(q);
    char key[sizeof g_pages[0].key];
    snprintf(key, sizeof key, "%s&offset=%d&limit=%d", path, offset, limit);
    cJSON *out = page_cache_get(key), *it;
    if (out) {
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(out, "items"))
            cJSON_ReplaceItemInObjectCaseSensitive(it, "installed", cJSON_CreateBool(rom_installed((int)jget_num(it, "id", 0))));
        return out;
    }
    out = cJSON_CreateObject();
    cJSON_AddArrayToObject(out, "items");
    long st = 0;
    if (romm_list(path, offset, limit, add_library_rom, out, &st) < 0) {
        if (st == 200) snprintf(err, (size_t)en, "could not read RomM's list of games (see the log)");
        else snprintf(err, (size_t)en, "could not load games (HTTP %ld)", st);
        cJSON_Delete(out);
        return NULL;
    }
    if (!cJSON_GetObjectItemCaseSensitive(out, "total")) cJSON_AddNumberToObject(out, "total", 0);
    page_cache_put(key, out);
    return out;
}
