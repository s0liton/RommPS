#include "clientcert.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "platform.h"
#include "util.h"

#define CERT_FILE "client.p12"
#define CERT_MAX  (256 * 1024)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_error[384]; /* why the configured certificate isn't in use */

static void set_error(const char *e) {
    pthread_mutex_lock(&g_lock);
    str_copy(g_error, sizeof g_error, e);
    pthread_mutex_unlock(&g_lock);
}

/* client_cert as a full path: relative ones are in the data folder. */
static void cert_path(const char *name, char *out, size_t n) {
    if (name[0] == '/') str_copy(out, n, name);
    else path_join(out, n, plat_data_dir(), name);
}

static void wipe(char *p, size_t len) {
    if (!p) return;
    volatile char *v = p;
    while (len--) *v++ = 0;
}

void clientcert_init(void) {
    char name[sizeof g_cfg.client_cert], pw[sizeof g_cfg.client_cert_password], path[PATH_MAX_LEN];
    config_lock();
    str_copy(name, sizeof name, g_cfg.client_cert);
    str_copy(pw, sizeof pw, g_cfg.client_cert_password);
    config_unlock();
    if (!name[0]) return;
    cert_path(name, path, sizeof path);
    size_t len = 0;
    char *data = read_file(path, &len);
    char err[300] = "", info[192] = "";
    if (!data) snprintf(err, sizeof err, "can't read %s", path);
    else if (http_client_cert_load(data, len, pw, 1, info, sizeof info, err, sizeof err) == 0)
        LOGI("client certificate %s in use (%s)", name, strstr(info, ", ") ? strstr(info, ", ") + 2 : info);
    wipe(pw, sizeof pw);
    free(data);
    if (err[0]) {
        char msg[sizeof g_error];
        snprintf(msg, sizeof msg, "client certificate %s: %s", name, err);
        LOGE("%s", msg);
        plat_notify("RommPS: the client certificate can't be used\n%s", err);
        set_error(msg);
    } else {
        set_error("");
    }
}

static int b64_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* malloc'd bytes, NULL if it isn't base64. Whitespace and padding are skipped. */
static unsigned char *b64_decode(const char *s, size_t *len) {
    size_t n = strlen(s), o = 0;
    unsigned char *out = malloc(n / 4 * 3 + 3);
    if (!out) return NULL;
    unsigned acc = 0;
    int bits = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r' || *p == ' ') continue;
        int v = b64_value((unsigned char)*p);
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
        }
    }
    *len = o;
    return out;
}

int clientcert_install(const char *p12_base64, const char *password, char *err, size_t en) {
    size_t len = 0;
    unsigned char *data = p12_base64 && p12_base64[0] ? b64_decode(p12_base64, &len) : NULL;
    if (!data || !len) {
        free(data);
        snprintf(err, en, "no certificate file was sent");
        return -1;
    }
    if (password && strlen(password) >= sizeof g_cfg.client_cert_password) {
        free(data);
        snprintf(err, en, "the password is too long (255 characters at most)");
        return -1;
    }
    if (len > CERT_MAX) {
        free(data);
        snprintf(err, en, "that file is too big for a certificate");
        return -1;
    }
    char info[192];
    if (http_client_cert_load(data, len, password, 0, info, sizeof info, err, en) != 0) {
        free(data);
        return -1;
    }
    char path[PATH_MAX_LEN];
    cert_path(CERT_FILE, path, sizeof path);
    int rc = write_file_atomic(path, data, len);
    if (rc != 0) {
        snprintf(err, en, "can't save %s", path);
    } else {
        config_lock();
        str_copy(g_cfg.client_cert, sizeof g_cfg.client_cert, CERT_FILE);
        str_copy(g_cfg.client_cert_password, sizeof g_cfg.client_cert_password, password ? password : "");
        rc = config_save();
        config_unlock();
        if (rc != 0) snprintf(err, en, "could not save config");
        else rc = http_client_cert_load(data, len, password, 1, NULL, 0, err, en);
    }
    free(data);
    if (rc == 0) {
        set_error("");
        LOGI("client certificate installed (%s)", strstr(info, ", ") ? strstr(info, ", ") + 2 : info);
    }
    return rc;
}

void clientcert_remove(void) {
    char name[sizeof g_cfg.client_cert];
    config_lock();
    str_copy(name, sizeof name, g_cfg.client_cert);
    g_cfg.client_cert[0] = 0;
    wipe(g_cfg.client_cert_password, sizeof g_cfg.client_cert_password);
    config_save();
    config_unlock();
    http_client_cert_load(NULL, 0, NULL, 1, NULL, 0, NULL, 0);
    /* Only the copy the upload made: one named by hand is the user's. */
    if (!strcmp(name, CERT_FILE)) {
        char path[PATH_MAX_LEN];
        cert_path(CERT_FILE, path, sizeof path);
        unlink(path);
    }
    set_error("");
    LOGI("client certificate removed");
}

cJSON *clientcert_status(void) {
    cJSON *j = cJSON_CreateObject();
    char info[192] = "", refused[256] = "";
    config_lock();
    cJSON_AddStringToObject(j, "file", g_cfg.client_cert);
    cJSON_AddBoolToObject(j, "password_set", g_cfg.client_cert_password[0] != 0);
    config_unlock();
    cJSON_AddBoolToObject(j, "active", http_client_cert_info(info, sizeof info));
    cJSON_AddStringToObject(j, "info", info);
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(j, "error", g_error);
    pthread_mutex_unlock(&g_lock);
    http_client_cert_refused(NULL, refused, sizeof refused);
    cJSON_AddStringToObject(j, "refused", refused);
    return j;
}
