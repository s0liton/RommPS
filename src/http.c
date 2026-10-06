#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "util.h"
#ifdef ROMM_EMBED_CA
#include "cacert_pem.h" /* generated from the SDK's Mozilla CA bundle */
#endif

static char g_ca_bundle[PATH_MAX_LEN];
/* Built-in roots plus the user's cacert.pem, if any. Only on the consoles,
 * which have no system CA store. */
static char *g_ca_blob;
static size_t g_ca_blob_len;
static int g_tls_verify = 1;
static char g_user_agent[64];

void http_global_init(const char *ca_bundle, int tls_verify) {
    curl_global_init(CURL_GLOBAL_ALL);
    snprintf(g_user_agent, sizeof g_user_agent, "%s/%s", plat_info()->client, APP_VERSION);
    if (ca_bundle && file_exists(ca_bundle)) str_copy(g_ca_bundle, sizeof g_ca_bundle, ca_bundle);
    g_tls_verify = tls_verify;
#ifdef ROMM_EMBED_CA
    /* A cacert.pem is added to the built-in roots, so a private CA works too. */
    size_t ulen = 0;
    char *user = g_ca_bundle[0] ? read_file(g_ca_bundle, &ulen) : NULL;
    g_ca_blob_len = sizeof CACERT_PEM - 1 + (user ? ulen + 1 : 0);
    g_ca_blob = malloc(g_ca_blob_len);
    if (g_ca_blob) {
        memcpy(g_ca_blob, CACERT_PEM, sizeof CACERT_PEM - 1);
        if (user) {
            g_ca_blob[sizeof CACERT_PEM - 1] = '\n';
            memcpy(g_ca_blob + sizeof CACERT_PEM, user, ulen);
        }
    }
    free(user);
    LOGI("libcurl %s, CA bundle: built-in%s%s", curl_version_info(CURLVERSION_NOW)->version,
         g_ca_bundle[0] ? " + " : "", g_ca_bundle);
#else
    LOGI("libcurl %s, CA bundle: %s", curl_version_info(CURLVERSION_NOW)->version,
         g_ca_bundle[0] ? g_ca_bundle : "(default)");
#endif
}

void http_set_tls_verify(int verify) { g_tls_verify = verify; }

void http_resp_free(http_resp *r) {
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

typedef struct {
    char *buf;
    size_t len, cap;
} membuf;

static size_t mem_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    membuf *m = ud;
    size_t n = size * nmemb;
    if (m->len + n + 1 > m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 4096;
        while (cap < m->len + n + 1) cap *= 2;
        char *nb = realloc(m->buf, cap);
        if (!nb) return 0;
        m->buf = nb;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, ptr, n);
    m->len += n;
    m->buf[m->len] = 0;
    return n;
}

static CURL *easy_new(const char *url, struct curl_slist **hdrs, const char *auth, int verify) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, g_user_agent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* Give up on stalled transfers rather than using a total timeout. */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    if (g_ca_blob) {
        struct curl_blob blob = {g_ca_blob, g_ca_blob_len, CURL_BLOB_NOCOPY};
        curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
    } else if (g_ca_bundle[0]) {
        curl_easy_setopt(c, CURLOPT_CAINFO, g_ca_bundle);
    }
    if (!verify) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    *hdrs = curl_slist_append(*hdrs, "Accept: application/json");
    if (auth && *auth) {
        char h[1024];
        snprintf(h, sizeof h, "Authorization: %s", auth);
        *hdrs = curl_slist_append(*hdrs, h);
    }
    return c;
}

static int finish(CURL *c, CURLcode rc, membuf *m, http_resp *out, const char *url) {
    memset(out, 0, sizeof *out);
    if (rc != CURLE_OK) {
        snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(rc));
        LOGW("HTTP %s: %s", url, out->error);
        free(m ? m->buf : NULL);
        return -1;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
    if (m) {
        out->body = m->buf;
        out->len = m->len;
    }
    if (out->status >= 400) {
        snprintf(out->error, sizeof out->error, "HTTP %ld", out->status);
        LOGD("HTTP %ld for %s: %.200s", out->status, url, out->body ? out->body : "");
    }
    return 0;
}

static int request_impl(const char *method, const char *url, const char *auth, const char *content_type,
                        const void *body, size_t body_len, int verify, http_resp *out) {
    struct curl_slist *hdrs = NULL;
    membuf m = {0};
    CURL *c = easy_new(url, &hdrs, auth, verify);
    if (!c) return -1;
    if (content_type) {
        char h[256];
        snprintf(h, sizeof h, "Content-Type: %s", content_type);
        hdrs = curl_slist_append(hdrs, h);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    if (strcmp(method, "GET") != 0) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)(body ? body_len : 0));
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    CURLcode rc = curl_easy_perform(c);
    int r = finish(c, rc, &m, out, url);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

int http_request(const char *method, const char *url, const char *auth, const char *content_type,
                 const void *body, size_t body_len, http_resp *out) {
    return request_impl(method, url, auth, content_type, body, body_len, g_tls_verify, out);
}

int http_get_verified(const char *url, http_resp *out) {
    return request_impl("GET", url, NULL, NULL, NULL, 0, 1, out);
}

int http_tls_untrusted(const char *url) {
    struct curl_slist *hdrs = NULL;
    CURL *c = easy_new(url, &hdrs, NULL, 1);
    if (!c) return 0;
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return rc == CURLE_PEER_FAILED_VERIFICATION;
}

int http_upload_file(const char *method, const char *url, const char *auth, const char *field,
                     const char *file_path, const char *upload_name, http_resp *out) {
    struct curl_slist *hdrs = NULL;
    membuf m = {0};
    CURL *c = easy_new(url, &hdrs, auth, g_tls_verify);
    if (!c) return -1;
    curl_mime *mime = curl_mime_init(c);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, field);
    curl_mime_filedata(part, file_path);
    curl_mime_filename(part, upload_name ? upload_name : path_basename(file_path));
    curl_mime_type(part, "application/octet-stream");
    /* "Expect: 100-continue" confuses some reverse proxies. Just don't. */
    hdrs = curl_slist_append(hdrs, "Expect:");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    if (strcmp(method, "POST") != 0) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    CURLcode rc = curl_easy_perform(c);
    int r = finish(c, rc, &m, out, url);
    curl_mime_free(mime);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

typedef struct {
    http_progress_fn fn;
    void *ud;
    int64_t offset;
} progress_ctx;

static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                   curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    progress_ctx *p = ud;
    if (!p->fn) return 0;
    return p->fn(p->ud, p->offset + dlnow, dltotal ? p->offset + dltotal : 0);
}

/* Downloads read and write in bigger blocks than curl's 16 KB, and ask for a
 * bigger socket buffer, which keeps a slow or bursty link (PS4 Wi-Fi) busier.
 * Kept modest: memory is tight for the PS4 payload. */
#define DOWNLOAD_BUFFER (256 * 1024)
#define DOWNLOAD_SOCKET_BUFFER (1024 * 1024)

static int big_rcvbuf(void *ud, curl_socket_t fd, curlsocktype purpose) {
    int size = DOWNLOAD_SOCKET_BUFFER;
    (void)ud;
    if (purpose == CURLSOCKTYPE_IPCXN) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    return CURL_SOCKOPT_OK;
}

static double mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int download_impl(const char *url, const char *auth, const char *dest, int resume, int verify,
                         http_progress_fn progress, void *ud, http_resp *out) {
    char part[PATH_MAX_LEN];
    snprintf(part, sizeof part, "%s.part", dest);
    mkdir_parent(dest);

    int64_t offset = resume ? file_size(part) : -1;
    if (offset < 0) offset = 0;
    FILE *f = fopen(part, offset > 0 ? "ab" : "wb");
    if (!f) {
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, "cannot open %s", part);
        return -1;
    }
    setvbuf(f, NULL, _IOFBF, DOWNLOAD_BUFFER);

    struct curl_slist *hdrs = NULL;
    CURL *c = easy_new(url, &hdrs, auth, verify);
    if (!c) {
        fclose(f);
        return -1;
    }
    progress_ctx pc = {progress, ud, offset};
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    if (offset > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pc);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)DOWNLOAD_BUFFER);
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, big_rcvbuf);
    double started = mono_sec();
    CURLcode rc = curl_easy_perform(c);
    fclose(f);
    double secs = mono_sec() - started;
    curl_off_t got = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &got);
    if (rc == CURLE_OK && got > 1024 * 1024 && secs > 0)
        LOGI("downloaded %.1f MB in %.1fs (%.2f MB/s)", got / 1048576.0, secs, got / 1048576.0 / secs);

    int r = finish(c, rc, NULL, out, url);
    if (r == 0 && out->status == 200 && offset > 0) {
        /* The server ignored Range, so start over. */
        LOGW("server ignored Range for %s; restarting download", url);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        unlink(part);
        return download_impl(url, auth, dest, 0, verify, progress, ud, out);
    }
    if (r == 0 && out->status >= 200 && out->status < 300) {
        if (move_file(part, dest) != 0) {
            snprintf(out->error, sizeof out->error, "cannot move into %s", dest);
            r = -1;
        }
    } else if (r == 0) {
        r = -1;
    }
    if (r != 0 && rc == CURLE_HTTP_RETURNED_ERROR) unlink(part);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

int http_download(const char *url, const char *auth, const char *dest, int resume,
                  http_progress_fn progress, void *ud, http_resp *out) {
    return download_impl(url, auth, dest, resume, g_tls_verify, progress, ud, out);
}

int http_download_verified(const char *url, const char *dest, http_progress_fn progress, void *ud,
                           http_resp *out) {
    return download_impl(url, NULL, dest, 0, 1, progress, ud, out);
}

char *http_escape(const char *s) {
    char *e = curl_easy_escape(NULL, s, 0);
    char *r = e ? strdup(e) : strdup("");
    curl_free(e);
    return r;
}
