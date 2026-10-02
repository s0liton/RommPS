/* RomM Sync for PS5. The main thread watches the foreground app and runs the
 * sync timer; the web server, sync, watcher and downloads have their own threads. */
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "detect.h"
#include "http.h"
#include "icon_png.h" /* generated from assets/icon0.png */
#include "library.h"
#include "platform.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "util.h"
#include "watch.h"
#include "web.h"

#define MONITOR_INTERVAL_SEC 3
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
    if (!strncmp(t, "ROMM0", 5)) return 0;
    return 1;
}

static void *tile_thread(void *arg) {
    if (plat_install_tile((int)(long)arg) == 0)
        LOGI("home screen tile ready");
    else
        LOGW("home screen tile not installed (unsupported or failed)");
    return NULL;
}

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

int main(void) {
    if (plat_init() != 0) fprintf(stderr, "platform init reported a problem\n");

    char path[PATH_MAX_LEN];
    mkdir_p(plat_data_dir());
    path_join(path, sizeof path, plat_data_dir(), "romm-sync.log");
    log_init(path);
    str_copy(g_crash_log, sizeof g_crash_log, path);
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGILL, on_crash);
    LOGI("%s %s starting (%s), data dir %s", APP_NAME, APP_VERSION, plat_name(), plat_data_dir());

    config_load();
    if (g_cfg.profiles_custom && detect_refresh_profiles(g_cfg.profiles) > 0) config_save();
    state_load();
    http_global_init(plat_ca_bundle(), g_cfg.tls_verify);

    sync_init();
    watch_init();
    library_init();
    int port = g_cfg.web_port;
    if (web_start(port) != 0) LOGE("web UI unavailable");

    /* Installing the tile takes about 25 s on the console, so it gets its own thread. */
    path_join(path, sizeof path, plat_data_dir(), "icon0.png");
    if (!file_exists(path)) write_file_atomic(path, ICON_PNG, sizeof ICON_PNG - 1);
    thread_start(tile_thread, (void *)(long)port);

    char ip[64] = "";
    plat_local_ip(ip, sizeof ip);
    if (config_is_paired()) {
        plat_notify("RomM Sync running - http://%s:%d", ip[0] ? ip : "127.0.0.1", port);
    } else {
        plat_notify("RomM Sync: open http://%s:%d to pair with RomM", ip[0] ? ip : "127.0.0.1", port);
    }

    sleep(5); /* wait for the network after boot */
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
    int have_title = 1, resume_ticks = 0;
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
                plat_notify("RomM Sync resumed - http://%s:%d", ip[0] ? ip : "127.0.0.1", port);
            }
        }

        char title[32] = "";
        int fg = have_title ? plat_running_title(title, sizeof title) : -1;
        if (fg < 0 && have_title) {
            LOGW("foreground app detection unsupported; relying on periodic sync");
            have_title = 0;
        }
        if (fg <= 0 || !is_game_title(title)) title[0] = 0;
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
    }
    return 0;
}
