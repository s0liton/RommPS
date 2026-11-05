/* RomM Sync for PS5 and PS4. The main thread watches the foreground app and runs the
 * sync timer; the web server, sync, watcher and downloads have their own threads. */
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "autostart.h"
#include "clientcert.h"
#include "config.h"
#include "events.h"
#include "covers.h"
#include "detect.h"
#include "http.h"
#include "library.h"
#include "platform.h"
#include "ra2.h"
#include "romm.h"
#include "memcard.h"
#include "state.h"
#include "sync.h"
#include "update.h"
#include "util.h"
#include "watch.h"
#include "web.h"
#include "presets.h"

#define MONITOR_INTERVAL_SEC 3
/* Failed foreground reads in a row before game detection is given up. */
#define MAX_TITLE_FAILURES 20
/* The loop ticks every few seconds, so a longer gap means the console slept. */
#define RESUME_GAP_SEC 60
/* Ticks to wait for the network after waking before notifying anyway. */
#define RESUME_NOTIFY_TICKS 10
/* Both resume signals usually fire on the same wake; notify only once. */
#define RESUME_NOTIFY_QUIET_SEC 120

/* System apps, the browser and our own tile don't count as games. */
static int is_game_title(const char *t) {
    if (!t || !t[0]) return 0;
    if (!strncmp(t, "NPXS", 4)) return 0;
    /* RomM Sync's own apps (RommPS on the PS5, the launcher on the PS4) aren't
     * games: while one is open, syncs, downloads and covers carry on. */
    if (!strcmp(t, "PPSA76677") || !strcmp(t, "ROMM00002")) return 0;
    if (!strncmp(t, "ROMM0", 5)) return 0;
    return 1;
}

/* AppInstUtil can take a while, so this gets its own thread. */
static void *tile_thread(void *arg) {
    (void)arg;
    plat_remove_old_tile();
    return NULL;
}


/* Lets the PS4 app read which version an installed payload is, so it never
 * replaces a newer one with the copy it carries (platform/ps4/app). */
__attribute__((used)) static const char VERSION_TAG[] = "ROMM_SYNC_VERSION=" APP_VERSION "\n";

static char g_crash_log[PATH_MAX_LEN];

/* If we end up here, something went very wrong. Leave a note in the log for
 * whoever reads it next (probably me). Only async-signal-safe calls allowed. */
static void on_crash(int sig) {
    int fd = open(g_crash_log, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd >= 0) {
        char msg[64] = "romm-sync crashed with signal ";
        size_t l = strlen(msg);
        if (sig >= 10) msg[l++] = (char)('0' + sig / 10);
        msg[l++] = (char)('0' + sig % 10);
        msg[l++] = '\n';
        write(fd, msg, l);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Emulators found on the console that setup never saw (installed later, or
 * new to this version): said once, so they don't go unnoticed. Ones the user
 * turned off still have a profile, and aren't news. */
static void announce_new_emulators(void) {
    if (!g_cfg.profiles_custom) return; /* setup offers them all anyway */
    cJSON *found = detect_emulators();
    char names[160] = "";
    int fresh = 0;
    const cJSON *c, *p;
    state_lock();
    cJSON *seen = state_section("emulators_seen");
    cJSON_ArrayForEach(c, found) {
        const char *id = jget_str(c, "id", jget_str(c, "kind", "")), *root = jget_str(c, "root", "");
        int known = cJSON_GetObjectItemCaseSensitive(seen, root) != NULL;
        config_lock();
        cJSON_ArrayForEach(p, g_cfg.profiles)
            known |= !strcmp(jget_str(p, "id", ""), id) || (root[0] && !strcmp(jget_str(p, "root", ""), root));
        config_unlock();
        if (known) continue;
        cJSON_AddTrueToObject(seen, root);
        const char *name = jget_str(c, "name", id);
        if (++fresh <= 3) snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", fresh > 1 ? ", " : "", name);
        events_add("emulator", "found", 0, root, name, "Turn it on in Emulator Settings");
    }
    if (fresh) state_save();
    state_unlock();
    cJSON_Delete(found);
    if (!fresh) return;
    events_save();
    LOGI("new emulators found: %s%s", names, fresh > 3 ? " and more" : "");
    if (fresh > 3) plat_notify("RommPS: found %s and %d more\nTurn them on in Emulator Settings", names, fresh - 3);
    else plat_notify("RommPS: found %s\nTurn %s on in Emulator Settings", names, fresh == 1 ? "it" : "them");
}

static void *app_main(void *arg) {
    (void)arg;
    int init = plat_init();
    if (init > 0) {
        /* RommPS and the HEN can both start a copy at boot; that's no news. */
        if (!plat_app_installed()) plat_notify("RommPS is already running");
        return NULL;
    }
    if (init != 0) fprintf(stderr, "platform init reported a problem\n");

    char path[PATH_MAX_LEN];
    mkdir_p(plat_data_dir());
    path_join(path, sizeof path, plat_data_dir(), "romm-sync.log");
    log_init(path);
    str_copy(g_crash_log, sizeof g_crash_log, path);
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGILL, on_crash);
    char about[256], now_iso[40];
    plat_describe(about, sizeof about);
    iso8601_utc(time(NULL), now_iso, sizeof now_iso);
    LOGI("%s %s (build %s) starting (%s), data dir %s", APP_NAME, APP_VERSION, APP_BUILD, plat_name(), plat_data_dir());
    LOGI("%s; clock %s; largest free block %d MB", about, now_iso, mem_largest_mb());
    /* Jailbroken consoles kept offline often run with the clock at 1970, which
     * breaks HTTPS certificate checks and save timestamps. */
    if (time(NULL) < 1577836800) { /* 2020-01-01 */
        LOGW("the console clock isn't set; HTTPS and save times will be wrong");
        plat_notify("RommPS: the console's date and time aren't set\nSet them in Settings to sync");
    }

    presets_init();
    config_load();
    if (g_cfg.profiles_custom && detect_refresh_profiles(g_cfg.profiles) > 0) config_save();
    state_load();
    /* PS2: consoles syncing shared cards keep them, now one per game is the default. */
    memcard_keep_shared();
    /* RetroArch 2.0 keeps games in new folders. */
    ra2_check(1);
    events_load();
    announce_new_emulators();
    /* Copies of older versions left on the console are brought up to date. */
    autostart_repair();
    http_global_init(plat_ca_bundle(), g_cfg.tls_verify);
    clientcert_init();

    sync_init();
    watch_init();
    library_init();
    covers_init();
    int port = g_cfg.web_port;
    if (web_start(port) != 0) LOGE("web UI unavailable");

    /* The old tile's icon, from versions before 1.1.1. */
    path_join(path, sizeof path, plat_data_dir(), "icon0.png");
    if (file_exists(path)) unlink(path);
    thread_start(tile_thread, NULL);

    char ip[64] = "";
    plat_local_ip(ip, sizeof ip);
    LOGI("web UI on http://%s:%d", ip[0] ? ip : "127.0.0.1", port);
    /* With RommPS installed, it's where the user sees all this. An older
     * RommPS, though, starts its own older copy now and then (1.1.0 does when
     * this one is slow to answer), so it's worth updating. */
    int quiet = plat_app_installed();
    char app_version[32];
    plat_app_version(app_version, sizeof app_version);
    if (quiet && app_version[0] && version_cmp(app_version, APP_VERSION) < 0) {
        LOGI("RommPS %s is older than RomM Sync %s", app_version, APP_VERSION);
        plat_notify("RommPS %s is out of date\nUpdate it from your homebrew store", app_version);
    } else if (quiet) {
    } else if (!plat_info()->self_update) {
        plat_notify("RommPS is running\nInstall the RommPS app from your homebrew store");
    } else if (config_is_paired()) {
        plat_notify("RommPS is running\nhttp://%s:%d", ip[0] ? ip : "127.0.0.1", port);
    } else {
        plat_notify("RommPS: open http://%s:%d to pair with RomM", ip[0] ? ip : "127.0.0.1", port);
    }

    /* The extras' mounts are watched for from the start: an emulator opened
     * right after boot gets them too, and ones an earlier copy left on come
     * off when it closes. */
    library_link_extras();

    sleep(5); /* wait for the network after boot */
    presets_check();

    if (config_is_paired()) {
        char base[512], version[32] = "";
        config_lock();
        str_copy(base, sizeof base, g_cfg.server_url);
        config_unlock();
        if (romm_heartbeat(base, version, sizeof version) == 0) {
            config_lock();
            str_copy(g_cfg.server_version, sizeof g_cfg.server_version, version);
            config_save();
            config_unlock();
        }
    }
    sync_request("startup");

    char current[32] = "";
    time_t game_start = 0, exit_at = 0, now, last_tick = time(NULL), resume_notified = 0;
    int title_failures = 0, resume_ticks = 0, rommps_seen = 0;
    for (;;) {
        sleep(MONITOR_INTERVAL_SEC);
        now = time(NULL);
        int resumed = 0;
        if (now - last_tick > RESUME_GAP_SEC) {
            LOGI("resumed from rest mode after %lds", (long)(now - last_tick));
            resumed = 1;
        }
        if (web_take_reopened()) resumed = 1;
        last_tick = now;
        if (resumed && !resume_ticks && now - resume_notified > RESUME_NOTIFY_QUIET_SEC)
            resume_ticks = RESUME_NOTIFY_TICKS;
        if (resume_ticks > 0) {
            plat_local_ip(ip, sizeof ip);
            if (ip[0] || --resume_ticks == 0) {
                resume_ticks = 0;
                resume_notified = now;
                if (!quiet) plat_notify("RommPS resumed\nhttp://%s:%d", ip[0] ? ip : "127.0.0.1", port);
            }
        }

        /* A failed read can be a passing thing (an app starting or closing),
         * so detection is only given up after a run of them. */
        char title[32] = "";
        int fg = title_failures < MAX_TITLE_FAILURES ? plat_running_title(title, sizeof title) : -1;
        if (fg >= 0) {
            title_failures = 0;
        } else if (title_failures < MAX_TITLE_FAILURES && ++title_failures == MAX_TITLE_FAILURES) {
            LOGW("can't tell which game is running (%d failed reads); relying on periodic sync", title_failures);
        }
        /* RommPS installed while this copy runs (someone coming from 1.0.2):
         * the old tile goes the first time it's opened, not at the next boot. */
        if (fg > 0 && !strcmp(title, "PPSA76677") && !rommps_seen) {
            rommps_seen = 1;
            thread_start(tile_thread, NULL);
        }
        if (fg <= 0 || !is_game_title(title)) title[0] = 0;
        /* Resting with a game open: nothing in front, but the game's
         * processes are all there. It hasn't closed: no "game exited" sync as
         * the console sleeps, no "game started" one as it wakes, and downloads
         * keep waiting for it. */
        if (!title[0] && current[0] && plat_app_alive(current) == 1) str_copy(title, sizeof title, current);
        web_set_foreground(title);
        sync_set_game_running(title[0] != 0);

        int on_exit, on_start, delay, interval;
        config_lock();
        on_exit = g_cfg.sync_on_game_exit;
        on_start = g_cfg.sync_on_game_start;
        delay = g_cfg.exit_delay_sec;
        interval = g_cfg.sync_interval_min;
        config_unlock();

        if (strcmp(title, current) != 0) {
            if (current[0]) {
                LOGI("game %s closed after %lds", current, (long)(now - game_start));
                sync_add_play_session(game_start, now, current);
                if (on_exit) exit_at = now + delay;
            }
            if (title[0]) {
                LOGI("game %s started", title);
                game_start = now;
                /* The emulator is usually still in its menu, so new saves can land. */
                if (on_start) sync_request("game started");
            }
            str_copy(current, sizeof current, title);
        }

        if (exit_at && now >= exit_at) {
            exit_at = 0;
            sync_request("game exited");
        }
        if (interval > 0 && !sync_is_running() && sync_last_run() &&
            now - sync_last_run() >= (time_t)interval * 60) {
            sync_request("periodic");
        }
        if (!sync_is_running() && sync_retry_due()) sync_request("retry");
        update_tick();
    }
    return NULL;
}

int main(void) {
    /* GoldHEN runs payloads on its loader's thread, which can't take another
     * payload until main() returns, so there the app gets its own thread. */
    if (plat_info()->detach && thread_start(app_main, NULL) == 0) return 0;
    app_main(NULL);
    return 0;
}
