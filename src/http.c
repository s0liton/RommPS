#include "http.h"
#include "jsonlean.h"

#include <pthread.h>

#include <curl/curl.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "util.h"
#ifdef ROMM_EMBED_CA
#include "cacert_pem.h" /* generated from the SDK's Mozilla CA bundle */
#endif
/* The consoles' libcurl is built on OpenSSL, and host builds use it when
 * it's there (the Makefile defines ROMM_OPENSSL): CA store, client certificates. */
#if defined(ROMM_EMBED_CA) || defined(ROMM_OPENSSL)
#define HAVE_OPENSSL 1
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif
#endif

static char g_ca_bundle[PATH_MAX_LEN];
/* Built-in roots plus the user's cacert.pem, if any. Only on the consoles,
 * which have no system CA store. */
static char *g_ca_blob;
static size_t g_ca_blob_len;
#ifdef ROMM_EMBED_CA
/* The same roots, parsed once and handed to every connection. Given as a
 * blob, libcurl parses all ~140 of them again for each new connection and
 * caches nothing, which the PS4's memory pool can't afford. */
static X509_STORE *g_ca_store;
#endif
static int g_tls_verify = 1;
static char g_user_agent[64];

/* mTLS: the client certificate (and its chain) and key from the user's .p12,
 * as PEM, handed to the RomM server's connections when it asks for one. Never
 * to the verified requests (GitHub). The key is kept in memory only. */
static pthread_mutex_t g_cc_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_cc_cert, *g_cc_key;
static size_t g_cc_cert_len, g_cc_key_len;
static char g_cc_info[192];
/* Why the server last turned the handshake down over the certificate, "" once it accepts one. */
static char g_cc_refused[256], g_cc_refused_origin[200];
/* The last TLS 1.2 check (cert_probe): which server, when. Forgotten when
 * the certificate changes, so the next refusal is told at once. */
static char g_probe_origin[200];
static double g_probe_at;

/* DNS answers and TLS sessions shared by every request, from any thread, so a
 * cover, a list page or a save skips the lookup and the full TLS handshake.
 * Not the connections themselves: libcurl doesn't support sharing those
 * between threads that use them at the same time. */
static CURLSH *g_share;
static pthread_mutex_t g_share_locks[CURL_LOCK_DATA_LAST];

/* The connections instead stay with the handles, which are kept between
 * requests: the next cover, list page or save to the same server goes over a
 * connection already open (two per handle at most), not a new one. A handle
 * can move from thread to thread as long as only one uses it at a time, so
 * each request takes an idle one (the last given back, whose connection is
 * the freshest) or a new one, and gives it back; POOL_MAX are kept. */
#define POOL_MAX 8
static CURL *g_pool[POOL_MAX];
static int pool_max(void) { return strcmp(plat_info()->name, "ps4") ? POOL_MAX : 3; }
static int g_npool;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static CURL *handle_take(void) {
    CURL *c = NULL;
    pthread_mutex_lock(&g_pool_lock);
    if (g_npool) c = g_pool[--g_npool];
    pthread_mutex_unlock(&g_pool_lock);
    if (!c) return curl_easy_init();
    curl_easy_reset(c); /* the options go; its connections stay */
    return c;
}

static void handle_give(CURL *c) {
    pthread_mutex_lock(&g_pool_lock);
    const int kept = g_npool < pool_max();
    if (kept) g_pool[g_npool++] = c;
    pthread_mutex_unlock(&g_pool_lock);
    if (!kept) curl_easy_cleanup(c);
}

static double mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* When a request last failed for want of memory: a reply we couldn't keep in
 * memory (a download's write error is the disk's), or curl or OpenSSL coming
 * up empty. */
static double g_oom_at = -1;
static pthread_mutex_t g_oom_lock = PTHREAD_MUTEX_INITIALIZER;

static int out_of_memory(CURLcode rc, int in_memory) {
    return rc == CURLE_OUT_OF_MEMORY || rc == CURLE_SSL_CACERT_BADFILE || (in_memory && rc == CURLE_WRITE_ERROR);
}

int http_out_of_memory(void) {
    pthread_mutex_lock(&g_oom_lock);
    const int recent = g_oom_at >= 0 && mono_sec() - g_oom_at < 30;
    pthread_mutex_unlock(&g_oom_lock);
    return recent;
}

/* A server that doesn't answer at all (no connection) isn't asked again by
 * every thread, each waiting out the connect timeout: requests to it fail at
 * once, and one at a time checks whether it's back, after 5 s, then twice as
 * long each time it still isn't, up to 30 s. */
#define DOWN_FIRST 5.0
#define DOWN_MOST  30.0
static pthread_mutex_t g_down_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_down[256]; /* its scheme://host:port, "" while all answer */
static double g_down_until, g_down_wait;
static int g_down_probing;

static void url_origin(const char *url, char *out, size_t n) {
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    snprintf(out, n, "%.*s", (int)(host - url + (ptrdiff_t)strcspn(host, "/?#")), url);
}

int http_down(const char *url) {
    char o[256];
    url_origin(url, o, sizeof o);
    pthread_mutex_lock(&g_down_lock);
    int down = g_down[0] && !strcmp(o, g_down);
    pthread_mutex_unlock(&g_down_lock);
    return down;
}

/* 0 to go ahead (*probe: this one checks whether it's back), -1 to fail at once. */
static int down_gate(const char *url, int *probe, http_resp *out) {
    char o[200];
    url_origin(url, o, sizeof o);
    *probe = 0;
    pthread_mutex_lock(&g_down_lock);
    int wait = 0;
    if (g_down[0] && !strcmp(o, g_down)) {
        if (g_down_probing || mono_sec() < g_down_until) wait = 1;
        else g_down_probing = *probe = 1;
    }
    pthread_mutex_unlock(&g_down_lock);
    if (!wait) return 0;
    memset(out, 0, sizeof *out);
    snprintf(out->error, sizeof out->error, "%s isn't answering", o);
    out->no_answer = 1;
    return -1;
}

static http_back_fn g_on_back;

void http_on_back(http_back_fn fn) { g_on_back = fn; }

static void down_note(const char *url, int no_answer, int probe) {
    char o[256];
    url_origin(url, o, sizeof o);
    int back = 0;
    pthread_mutex_lock(&g_down_lock);
    const int same = !strcmp(o, g_down);
    if (no_answer) {
        if (!same) {
            str_copy(g_down, sizeof g_down, o);
            g_down_wait = DOWN_FIRST;
            LOGW("%s isn't answering: asking again in %.0f s", o, g_down_wait);
        } else if (probe) {
            g_down_wait = g_down_wait * 2 > DOWN_MOST ? DOWN_MOST : g_down_wait * 2;
        }
        g_down_until = mono_sec() + g_down_wait;
    } else if (same) {
        LOGI("%s answers again", o);
        g_down[0] = 0;
        back = 1;
    }
    if (probe) g_down_probing = 0;
    pthread_mutex_unlock(&g_down_lock);
    if (back && g_on_back) g_on_back(o);
}

/* No connection at all: refused, no such host, or the connect timeout. */
static int no_answer(CURL *c, CURLcode rc) {
    if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST) return 1;
    if (rc != CURLE_OPERATION_TIMEDOUT) return 0;
    /* Timed out before a single line of an answer: a server that's hung
     * (paused, wedged) takes the connection and then says nothing, which is
     * as good as not being there. One that's slow mid-answer isn't that. */
    long status = 0, headers = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_HEADER_SIZE, &headers);
    return status == 0 && headers == 0;
}

static void share_lock(CURL *c, curl_lock_data data, curl_lock_access access, void *ud) {
    (void)c, (void)access, (void)ud;
    if (data < CURL_LOCK_DATA_LAST) pthread_mutex_lock(&g_share_locks[data]);
}

static void share_unlock(CURL *c, curl_lock_data data, void *ud) {
    (void)c, (void)ud;
    if (data < CURL_LOCK_DATA_LAST) pthread_mutex_unlock(&g_share_locks[data]);
}

#ifdef ROMM_EMBED_CA
/* What libcurl does with a CA blob (load_cacert_from_memory and the flags it
 * sets), done once. NULL if not one certificate in it could be read. */
static X509_STORE *ca_store(const char *pem, size_t len) {
    X509_STORE *st = X509_STORE_new();
    BIO *bio = st && len <= 0x7fffffff ? BIO_new_mem_buf(pem, (int)len) : NULL;
    STACK_OF(X509_INFO) *infos = bio ? PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL) : NULL;
    int n = 0;
    for (int i = 0; infos && i < sk_X509_INFO_num(infos); i++) {
        X509_INFO *inf = sk_X509_INFO_value(infos, i);
        if (inf->x509 && X509_STORE_add_cert(st, inf->x509)) n++;
    }
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    BIO_free(bio);
    if (!n) {
        X509_STORE_free(st);
        return NULL;
    }
    X509_STORE_set_flags(st, X509_V_FLAG_TRUSTED_FIRST | X509_V_FLAG_PARTIAL_CHAIN);
    return st;
}

/* Every connection's SSL_CTX gets the one store; libcurl's own, empty, goes. */
static CURLcode use_ca_store(CURL *c, void *ssl_ctx, void *ud) {
    (void)c;
    if (!X509_STORE_up_ref(ud)) return CURLE_OUT_OF_MEMORY;
    SSL_CTX_set_cert_store(ssl_ctx, ud);
    return CURLE_OK;
}
#endif

void http_global_init(const char *ca_bundle, int tls_verify) {
    curl_global_init(CURL_GLOBAL_ALL);
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&g_share_locks[i], NULL);
    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
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
    const char *tls = curl_version_info(CURLVERSION_NOW)->ssl_version;
    if (g_ca_blob && tls && !strncmp(tls, "OpenSSL", 7) && (g_ca_store = ca_store(g_ca_blob, g_ca_blob_len))) {
        free(g_ca_blob);
        g_ca_blob = NULL;
    }
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

#ifdef ROMM_NETLOG
/* NETLOG=1 builds: curl's running commentary (DNS, connect, TLS, headers) goes
 * to the network log only, so a timeout says which step it died in. Headers
 * that carry credentials or session cookies are left out. */
static int curl_trace(CURL *c, curl_infotype type, char *data, size_t size, void *ud) {
    static const char *const tags[] = {[CURLINFO_TEXT] = "*", [CURLINFO_HEADER_IN] = "<", [CURLINFO_HEADER_OUT] = ">"};
    char line[300];
    (void)c;
    (void)ud;
    if (type != CURLINFO_TEXT && type != CURLINFO_HEADER_IN && type != CURLINFO_HEADER_OUT) return 0;
    while (size > 0) {
        size_t n = 0;
        while (n < size && data[n] != '\n') n++;
        size_t take = n;
        while (take > 0 && data[take - 1] == '\r') take--;
        int secret = !strncasecmp(data, "Authorization:", 14) || !strncasecmp(data, "Cookie:", 7) ||
                     !strncasecmp(data, "Set-Cookie:", 11);
        if (take && !(type != CURLINFO_TEXT && secret)) {
            snprintf(line, sizeof line, "curl %s %.*s", tags[type], (int)(take > 280 ? 280 : take), data);
            netlog_send(line);
        }
        if (n < size) n++;
        data += n;
        size -= n;
    }
    return 0;
}
#endif

#if defined(__FreeBSD__)
/* The socket's receive buffer is the TCP window, and the window caps a
 * download at window / round trip, whatever the line can do. The consoles grow
 * it by themselves only from 64 KB to 128 KB: about 2.4 MB/s from a server
 * 40 ms away (issue #39). So it's set by hand, on every connection: a download
 * usually goes over the one the API call before it left open, and setting it
 * on download connections alone missed nearly all of them. The PS5 quietly
 * hands out 512 KB at most for anything up to 4 MB and refuses 8 MB outright,
 * so ask big and step down until one is taken; the PS4 stops at 1 MB, its
 * memory being tight. Buffer memory is only used by data waiting to be read,
 * so the API's connections cost nothing for it. Linux grows the buffer by
 * itself as far as it's needed and stops once it's set by hand, so there it's
 * left alone. */
static int g_rcvbuf; /* the size the kernel took, once known */

static int big_rcvbuf(void *ud, curl_socket_t fd, curlsocktype purpose) {
    (void)ud;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    int size = __atomic_load_n(&g_rcvbuf, __ATOMIC_RELAXED);
    if (size) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
        return CURL_SOCKOPT_OK;
    }
    for (size = strcmp(plat_info()->name, "ps4") ? 8 * 1024 * 1024 : 1024 * 1024; size >= 128 * 1024; size /= 2)
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) == 0) break;
    if (size < 128 * 1024) return CURL_SOCKOPT_OK; /* none taken: the default stays, and the next one asks again */
    int got = 0;
    socklen_t gl = sizeof got;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &gl);
    if (!__atomic_exchange_n(&g_rcvbuf, size, __ATOMIC_RELAXED))
        LOGI("receive window: %d KB (%d KB asked)", got / 1024, size / 1024);
    return CURL_SOCKOPT_OK;
}
#endif

/* errbuf, CURL_ERROR_SIZE bytes or NULL, gets curl's own account of a
 * failure: which host, which phase, how long it waited. client: a request to
 * the RomM server, which gets the client certificate, if there is one. */
static CURL *easy_new(const char *url, struct curl_slist **hdrs, const char *auth, int verify, int client, char *errbuf) {
    CURL *c = handle_take();
    if (!c) return NULL;
    if (errbuf) {
        errbuf[0] = 0;
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    }
    if (g_share) curl_easy_setopt(c, CURLOPT_SHARE, g_share);
#if defined(__FreeBSD__)
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, big_rcvbuf);
#endif
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, g_user_agent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_MAXCONNECTS, 2L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* Give up on stalled transfers rather than using a total timeout. */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
#ifdef ROMM_NETLOG
    curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, curl_trace);
    curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
#endif
#ifdef ROMM_EMBED_CA
    if (g_ca_store) {
        curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
        curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
        curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, use_ca_store);
        curl_easy_setopt(c, CURLOPT_SSL_CTX_DATA, g_ca_store);
    } else
#endif
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
    if (client) {
        pthread_mutex_lock(&g_cc_lock);
        if (g_cc_cert) {
            /* Copied into the handle: the certificate can change meanwhile. */
            struct curl_blob cert = {g_cc_cert, g_cc_cert_len, CURL_BLOB_COPY};
            struct curl_blob key = {g_cc_key, g_cc_key_len, CURL_BLOB_COPY};
            curl_easy_setopt(c, CURLOPT_SSLCERTTYPE, "PEM");
            curl_easy_setopt(c, CURLOPT_SSLKEYTYPE, "PEM");
            CURLcode e = curl_easy_setopt(c, CURLOPT_SSLCERT_BLOB, &cert);
            if (e == CURLE_OK) e = curl_easy_setopt(c, CURLOPT_SSLKEY_BLOB, &key);
            /* A libcurl whose TLS library can't take them from memory (GnuTLS
             * builds on a computer; never the consoles') would go without. */
            static int told;
            if (e != CURLE_OK && !told) {
                told = 1;
                LOGE("this libcurl can't send the client certificate: %s", curl_easy_strerror(e));
            }
        }
        pthread_mutex_unlock(&g_cc_lock);
    }
    *hdrs = curl_slist_append(*hdrs, "Accept: application/json");
    if (auth && *auth) {
        char h[1024];
        snprintf(h, sizeof h, "Authorization: %s", auth);
        *hdrs = curl_slist_append(*hdrs, h);
    }
    return c;
}

static void cert_note(const char *url, CURLcode rc, const char *errbuf, int may_probe);
static void cert_forget(const char *url);

static int finish(CURL *c, CURLcode rc, membuf *m, http_resp *out, const char *url, const char *errbuf, int probe,
                  int client) {
    memset(out, 0, sizeof *out);
    /* Where a refused client certificate shows: in the handshake, or right
     * after it (TLS 1.3), before any answer. */
    const int tls_err = rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR;
    if (client) {
        /* nginx finishes the handshake without a certificate and says so in a
         * 400 page instead: taken as the alert it didn't send. */
        long st = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
        const char *page = rc == CURLE_OK && st == 400 && m && m->buf ? m->buf : "";
        const char *alert = strstr(page, "No required SSL certificate was sent") ? "alert certificate required"
                            : strstr(page, "The SSL certificate error")          ? "alert bad certificate"
                                                                                 : NULL;
        if (alert) cert_note(url, CURLE_SSL_CONNECT_ERROR, alert, 0);
        /* Not there at all: an older refusal no longer says what's wrong. */
        else if (rc != CURLE_OK && no_answer(c, rc)) cert_forget(url);
        /* Asked again only when it broke before any answer: a connection lost
         * halfway through a download isn't the certificate's doing. */
        else cert_note(url, rc, errbuf, st == 0);
    }
    out->no_answer = rc != CURLE_OK && no_answer(c, rc);
    out->aborted = rc == CURLE_ABORTED_BY_CALLBACK;
    down_note(url, out->no_answer, probe);
    if (rc != CURLE_OK) {
        snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(rc));
        if (out_of_memory(rc, m != NULL)) {
            pthread_mutex_lock(&g_oom_lock);
            g_oom_at = mono_sec();
            pthread_mutex_unlock(&g_oom_lock);
        }
        /* A download (CURLOPT_FAILONERROR) stopped by the server's answer:
         * say which, so a 404 isn't "HTTP response code said error". */
        if (rc == CURLE_HTTP_RETURNED_ERROR) {
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
            snprintf(out->error, sizeof out->error, "HTTP %ld", out->status);
        }
        /* "Timeout was reached" alone can't tell a server that never answered
         * from one that went quiet halfway, or a console with no network. */
        long os_err = 0;
        curl_easy_getinfo(c, CURLINFO_OS_ERRNO, &os_err);
        const char *detail = errbuf && errbuf[0] ? errbuf : out->error;
        if (client && tls_err) http_client_cert_refused(url, out->error, sizeof out->error); /* in plain words */
        /* curl's words for a reply we had no room for don't say so. */
        const char *oom = out_of_memory(rc, m != NULL) && rc != CURLE_OUT_OF_MEMORY ? " (out of memory)" : "";
        if (os_err) LOGW("HTTP %s: %s%s (errno %ld: %s)", url, detail, oom, os_err, strerror((int)os_err));
        else LOGW("HTTP %s: %s%s", url, detail, oom);
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

/* A reply kept lean: the members named in drop never reach memory. */
typedef struct {
    membuf m;
    jlean j;
} lean_buf;

static int lean_put(void *ud, const char *bytes, size_t n) {
    return mem_write((char *)bytes, 1, n, ud) == n;
}

static size_t lean_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    lean_buf *l = ud;
    return jlean_feed(&l->j, ptr, size * nmemb, lean_put, &l->m) ? size * nmemb : 0;
}

static int request_impl(const char *method, const char *url, const char *auth, const char *content_type,
                        const void *body, size_t body_len, int verify, int client, const char *const *drop,
                        http_resp *out) {
    memset(out, 0, sizeof *out);
    int probe;
    if (down_gate(url, &probe, out)) return -1;
    struct curl_slist *hdrs = NULL;
    membuf m = {0};
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = easy_new(url, &hdrs, auth, verify, client, errbuf);
    if (!c) {
        down_note(url, 0, probe);
        return -1;
    }
    if (content_type) {
        char h[256];
        snprintf(h, sizeof h, "Content-Type: %s", content_type);
        hdrs = curl_slist_append(hdrs, h);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    /* Compressed answers where the server offers them: a page of RomM's games
     * is about 1.4 MB of JSON, and a seventh of that gzipped. */
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (strcmp(method, "GET") != 0) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)(body ? body_len : 0));
    }
    lean_buf lean;
    if (drop) {
        memset(&lean, 0, sizeof lean);
        jlean_init(&lean.j, drop);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, lean_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &lean);
    } else {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    }
    CURLcode rc = curl_easy_perform(c);
    if (drop) m = lean.m;
    int r = finish(c, rc, &m, out, url, errbuf, probe, client);
    curl_slist_free_all(hdrs);
    handle_give(c);
    return r;
}

int http_request(const char *method, const char *url, const char *auth, const char *content_type,
                 const void *body, size_t body_len, http_resp *out) {
    return request_impl(method, url, auth, content_type, body, body_len, g_tls_verify, 1, NULL, out);
}

int http_get_lean(const char *url, const char *auth, const char *const *drop, http_resp *out) {
    return request_impl("GET", url, auth, NULL, NULL, 0, g_tls_verify, 1, drop, out);
}

int http_get_verified(const char *url, http_resp *out) {
    return request_impl("GET", url, NULL, NULL, NULL, 0, 1, 0, NULL, out);
}

int http_tls_untrusted(const char *url) {
    struct curl_slist *hdrs = NULL;
    CURL *c = easy_new(url, &hdrs, NULL, 1, 1, NULL);
    if (!c) return 0;
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    handle_give(c);
    return rc == CURLE_PEER_FAILED_VERIFICATION;
}

int http_upload_file(const char *method, const char *url, const char *auth, const char *field,
                     const char *file_path, const char *upload_name, http_resp *out) {
    int probe;
    if (down_gate(url, &probe, out)) return -1;
    struct curl_slist *hdrs = NULL;
    membuf m = {0};
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = easy_new(url, &hdrs, auth, g_tls_verify, 1, errbuf);
    if (!c) {
        down_note(url, 0, probe);
        return -1;
    }
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
    int r = finish(c, rc, &m, out, url, errbuf, probe, 1);
    curl_mime_free(mime);
    curl_slist_free_all(hdrs);
    handle_give(c);
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

/* Downloads read and write in bigger blocks than curl's 16 KB. */
#define DOWNLOAD_BUFFER (256 * 1024)

/* One connection carries at most its receive window per round trip, and the
 * PS5 holds that window to 512 KB: about 12 MB/s from a server 40 ms away,
 * a fifth of what the line can do (issue #39; measured on such a line, six
 * connections of 512 KB carried its full 62 MB/s). So a big download goes
 * over several connections at once, PAR_PIECE at a time, each piece written
 * in its place in "<dest>.par.part". That file has holes until it's whole: one
 * that stops halfway is cut back to where it's whole and becomes the .part a
 * resume carries on from; one a crash leaves is never taken for <dest>.part,
 * yet ends in .part, so it is skipped and cleared like any other.
 * Small files stay on one connection, with no request more. Not on the PS4,
 * its memory being tight. */
#define PAR_CONNS 6
#define PAR_PIECE ((int64_t)16 * 1024 * 1024)

static int par_conns(void) { return strcmp(plat_info()->name, "ps4") ? PAR_CONNS : 1; }

typedef struct {
    const char *url, *auth;
    int verify, client, fd;
    int64_t base, total; /* where the pieces start (what the .part had), the file's size */
    int npieces, next;   /* next: the first piece no connection took yet */
    int64_t *written;    /* bytes in place per piece, each filled from its start */
    int64_t got;         /* all of them, for the progress */
    int stop, failed, running; /* stop: the user's, at once; failed: no new piece */
    int cut;                   /* the first failed piece: the ones after it would be cut away */
    http_resp err; /* the first failure's */
    pthread_mutex_t lock;
} par_dl;

typedef struct {
    par_dl *d;
    CURL *c;
    int piece;
    int64_t at, end; /* the next byte's place in the file, the piece's end */
} par_conn;

static int64_t piece_len(const par_dl *d, int i) {
    const int64_t left = d->total - d->base - (int64_t)i * PAR_PIECE;
    return left < PAR_PIECE ? left : PAR_PIECE;
}

static size_t par_write(char *ptr, size_t size, size_t n, void *ud) {
    par_conn *w = ud;
    const size_t len = size * n;
    long st = 0;
    curl_easy_getinfo(w->c, CURLINFO_RESPONSE_CODE, &st);
    /* The piece asked for and no more: a whole file sent instead would land
     * on the pieces after it. */
    if (st != 206 || w->at + (int64_t)len > w->end) return 0;
    if (pwrite(w->d->fd, ptr, len, (off_t)w->at) != (ssize_t)len) return 0;
    w->at += (int64_t)len;
    pthread_mutex_lock(&w->d->lock);
    w->d->written[w->piece] += (int64_t)len;
    w->d->got += (int64_t)len;
    pthread_mutex_unlock(&w->d->lock);
    return len;
}

static int par_xfer(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
    const par_conn *w = ud;
    return __atomic_load_n(&w->d->stop, __ATOMIC_RELAXED) || w->piece > __atomic_load_n(&w->d->cut, __ATOMIC_RELAXED);
}

static void *par_conn_run(void *arg) {
    par_conn *w = arg;
    par_dl *d = w->d;
    struct curl_slist *hdrs = NULL;
    char errbuf[CURL_ERROR_SIZE];
    w->c = easy_new(d->url, &hdrs, d->auth, d->verify, d->client, errbuf);
    if (w->c) {
        curl_easy_setopt(w->c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(w->c, CURLOPT_WRITEFUNCTION, par_write);
        curl_easy_setopt(w->c, CURLOPT_WRITEDATA, w);
        curl_easy_setopt(w->c, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(w->c, CURLOPT_BUFFERSIZE, (long)DOWNLOAD_BUFFER);
        curl_easy_setopt(w->c, CURLOPT_XFERINFOFUNCTION, par_xfer);
        curl_easy_setopt(w->c, CURLOPT_XFERINFODATA, w);
        curl_easy_setopt(w->c, CURLOPT_NOPROGRESS, 0L);
    }
    while (w->c) {
        pthread_mutex_lock(&d->lock);
        /* After a failure, no new piece: the ones under way before it finish,
         * so the file gets further; the ones after it stop (par_xfer), being
         * cut away anyway. */
        const int i = __atomic_load_n(&d->stop, __ATOMIC_RELAXED) || d->failed ? d->npieces : d->next++;
        pthread_mutex_unlock(&d->lock);
        if (i >= d->npieces) break;
        w->piece = i;
        w->at = d->base + (int64_t)i * PAR_PIECE;
        w->end = w->at + piece_len(d, i);
        char range[48];
        snprintf(range, sizeof range, "%lld-%lld", (long long)w->at, (long long)w->end - 1);
        curl_easy_setopt(w->c, CURLOPT_RANGE, range);
        CURLcode rc = curl_easy_perform(w->c);
        if (rc == CURLE_OK && w->at == w->end) continue;
        if (rc == CURLE_OK) rc = CURLE_PARTIAL_FILE; /* the server stopped short */
        if (!__atomic_load_n(&d->stop, __ATOMIC_RELAXED)) { /* not the user's stop */
            pthread_mutex_lock(&d->lock);
            const int first = !d->failed;
            d->failed = 1;
            if (i < d->cut) __atomic_store_n(&d->cut, i, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&d->lock);
            if (first) finish(w->c, rc, NULL, &d->err, d->url, errbuf, 0, d->client);
        }
        break;
    }
    curl_slist_free_all(hdrs);
    if (w->c) handle_give(w->c);
    pthread_mutex_lock(&d->lock);
    d->running--;
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

/* What a download's first answer says (Content-Range: bytes <from>-<to>/<size>),
 * and where its bytes go until the file turns out big: then it's handed over
 * to par_download before a byte is written. */
typedef struct {
    FILE *f;
    int64_t offset, total; /* total: -1 until a Content-Range says it */
    int par, handed;       /* par: big ones may go over several connections */
} dl_ctx;

static size_t dl_header(char *b, size_t size, size_t n, void *ud) {
    const size_t len = size * n;
    char h[128];
    long long a, z, total;
    if (len < sizeof h) {
        memcpy(h, b, len);
        h[len] = 0;
        /* A 416 gives the size alone, after a star. */
        if (!strncasecmp(h, "content-range:", 14) && (sscanf(h + 14, " bytes %lld-%lld/%lld", &a, &z, &total) == 3 ||
                                                      sscanf(h + 14, " bytes */%lld", &total) == 1))
            ((dl_ctx *)ud)->total = total;
    }
    return len;
}

static size_t dl_write(char *p, size_t size, size_t n, void *ud) {
    dl_ctx *k = ud;
    if (k->par && k->total - k->offset > PAR_PIECE) {
        k->handed = 1;
        return 0; /* stops this transfer */
    }
    return fwrite(p, size, n, k->f);
}

/* The rest of a file from offset (what its .part has) on, total bytes in all. */
static int par_download(const char *url, const char *auth, const char *dest, const char *part, int64_t offset,
                        int64_t total, int verify, int client, http_progress_fn progress, void *ud, http_resp *out) {
    char par[PATH_MAX_LEN];
    snprintf(par, sizeof par, "%s.par.part", dest);
    const int fd = rename(part, par) == 0 ? open(par, O_WRONLY) : -1;
    par_dl d = {.url = url, .auth = auth, .verify = verify, .client = client, .fd = fd, .base = offset,
                .total = total};
    d.npieces = d.cut = (int)((total - offset + PAR_PIECE - 1) / PAR_PIECE);
    d.written = fd >= 0 ? calloc((size_t)d.npieces, sizeof *d.written) : NULL;
    if (!d.written) {
        if (fd >= 0) close(fd);
        rename(par, part);
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, "cannot open %s", par);
        return -1;
    }
    pthread_mutex_init(&d.lock, NULL);
    par_conn w[PAR_CONNS] = {{0}};
    pthread_t t[PAR_CONNS];
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1024 * 1024);
    int n = 0;
    const double started = mono_sec();
    /* Held while they start: one that ends at once can't count down before it's counted. */
    pthread_mutex_lock(&d.lock);
    for (int i = 0; i < par_conns() && i < d.npieces; i++) {
        w[n].d = &d;
        if (pthread_create(&t[n], &a, par_conn_run, &w[n]) == 0) n++;
    }
    d.running = n;
    pthread_mutex_unlock(&d.lock);
    pthread_attr_destroy(&a);
    int aborted = 0;
    for (;;) {
        pthread_mutex_lock(&d.lock);
        const int running = d.running, stop = __atomic_load_n(&d.stop, __ATOMIC_RELAXED);
        const int64_t got = d.got;
        pthread_mutex_unlock(&d.lock);
        if (!running) break;
        if (!stop && progress && progress(ud, offset + got, total)) {
            aborted = 1;
            __atomic_store_n(&d.stop, 1, __ATOMIC_RELAXED);
        }
        usleep(200 * 1000);
    }
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    pthread_mutex_destroy(&d.lock);

    int64_t whole = offset; /* from the start, without a gap */
    for (int i = 0; i < d.npieces; i++) {
        whole += d.written[i];
        if (d.written[i] < piece_len(&d, i)) break;
    }
    free(d.written);
    if (whole == total) {
        close(fd);
        const double secs = mono_sec() - started;
        const double mb = (double)(total - offset) / 1048576.0;
        LOGI("downloaded %.1f MB in %.1fs (%.2f MB/s) over %d connections", mb, secs, secs > 0 ? mb / secs : 0, n);
        memset(out, 0, sizeof *out);
        out->status = 200;
        if (move_file(par, dest) == 0) return 0;
        snprintf(out->error, sizeof out->error, "cannot move into %s", dest);
        return -1;
    }
    /* Kept up to where it's whole, for the next go to carry on from; not
     * after the server refused it (a 404). */
    if (d.failed && !aborted && d.err.status >= 400) {
        close(fd);
        unlink(par);
    } else {
        const int kept = whole > 0 && ftruncate(fd, (off_t)whole) == 0;
        close(fd);
        if (!kept || rename(par, part) != 0) unlink(par);
    }
    if (d.failed && !aborted) {
        *out = d.err;
    } else {
        memset(out, 0, sizeof *out);
        out->aborted = aborted;
        snprintf(out->error, sizeof out->error, "%s",
                 aborted ? curl_easy_strerror(CURLE_ABORTED_BY_CALLBACK) : "download stopped");
    }
    return -1;
}

static int download_impl(const char *url, const char *auth, const char *dest, int resume, int verify, int client,
                         http_progress_fn progress, void *ud, http_resp *out) {
    int probe;
    if (down_gate(url, &probe, out)) return -1;
    char part[PATH_MAX_LEN];
    snprintf(part, sizeof part, "%s.part", dest);
    mkdir_parent(dest);

    int64_t offset = resume ? file_size(part) : -1;
    if (offset < 0) offset = 0;
    FILE *f = fopen(part, offset > 0 ? "ab" : "wb");
    if (!f) {
        down_note(url, 0, probe);
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, "cannot open %s", part);
        return -1;
    }
    setvbuf(f, NULL, _IOFBF, DOWNLOAD_BUFFER);

    struct curl_slist *hdrs = NULL;
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = easy_new(url, &hdrs, auth, verify, client, errbuf);
    if (!c) {
        down_note(url, 0, probe);
        fclose(f);
        return -1;
    }
    progress_ctx pc = {progress, ud, offset};
    dl_ctx k = {f, offset, -1, client && par_conns() > 1, 0};
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, dl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &k);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &k);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    if (offset > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);
    else if (k.par) curl_easy_setopt(c, CURLOPT_RANGE, "0-"); /* so the answer says the file's size */
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pc);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)DOWNLOAD_BUFFER);
    double started = mono_sec();
    CURLcode rc = curl_easy_perform(c);
    fclose(f);
    if (k.handed) {
        finish(c, CURLE_OK, NULL, out, url, errbuf, probe, client); /* it answered */
        curl_slist_free_all(hdrs);
        handle_give(c);
        return par_download(url, auth, dest, part, offset, k.total, verify, client, progress, ud, out);
    }
    double secs = mono_sec() - started;
    curl_off_t got = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &got);
    if (rc == CURLE_OK && got > 1024 * 1024 && secs > 0)
        LOGI("downloaded %.1f MB in %.1fs (%.2f MB/s)", got / 1048576.0, secs, got / 1048576.0 / secs);

    /* "0-" can't be satisfied for an empty file: the answer is that file. */
    const int empty = rc == CURLE_HTTP_RETURNED_ERROR && k.par && offset == 0 && k.total == 0;
    int r = finish(c, empty ? CURLE_OK : rc, NULL, out, url, errbuf, probe, client);
    if (empty) {
        out->status = 200;
        out->error[0] = 0;
    }
    if (r == 0 && out->status == 200 && offset > 0) {
        /* The server ignored Range, so start over. */
        LOGW("server ignored Range for %s; restarting download", url);
        curl_slist_free_all(hdrs);
        handle_give(c);
        unlink(part);
        return download_impl(url, auth, dest, 0, verify, client, progress, ud, out);
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
    handle_give(c);
    return r;
}

int http_download(const char *url, const char *auth, const char *dest, int resume,
                  http_progress_fn progress, void *ud, http_resp *out) {
    return download_impl(url, auth, dest, resume, g_tls_verify, 1, progress, ud, out);
}

int http_download_verified(const char *url, const char *dest, http_progress_fn progress, void *ud,
                           http_resp *out) {
    return download_impl(url, NULL, dest, 0, 1, 0, progress, ud, out);
}

char *http_escape(const char *s) {
    char *e = curl_easy_escape(NULL, s, 0);
    char *r = e ? strdup(e) : strdup("");
    curl_free(e);
    return r;
}

/* ---- mTLS client certificate ---- */

/* The TLS alert a server sends when it wants a client certificate, or won't
 * take the one it got (OpenSSL's words, in curl's error): NULL if not one. */
static const char *cert_alert(const char *errbuf) {
    static const char *const alerts[] = {"certificate required", "bad certificate",   "unknown ca",
                                         "certificate expired",  "certificate revoked", "certificate unknown",
                                         "unsupported certificate", "access denied"};
    const char *a = errbuf ? strstr(errbuf, "alert ") : NULL;
    if (!a) return NULL;
    for (size_t i = 0; i < sizeof alerts / sizeof alerts[0]; i++)
        if (!strncasecmp(a + 6, alerts[i], strlen(alerts[i]))) return alerts[i];
    return NULL;
}

/* With TLS 1.3 a server checks the client certificate after the handshake,
 * and its alert often comes when the request is already on its way: curl
 * sees only a broken pipe or a reset. With TLS 1.2 the alert ends the
 * handshake itself, so a connection like that tells what went wrong. At most
 * every 30 s per server. */
static void cert_probe(const char *url) {
    char o[200];
    url_origin(url, o, sizeof o);
    pthread_mutex_lock(&g_cc_lock);
    const int skip = !strcmp(o, g_probe_origin) && mono_sec() - g_probe_at < 30;
    if (!skip) {
        str_copy(g_probe_origin, sizeof g_probe_origin, o);
        g_probe_at = mono_sec();
    }
    pthread_mutex_unlock(&g_cc_lock);
    if (skip) return;
    struct curl_slist *hdrs = NULL;
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = easy_new(url, &hdrs, NULL, g_tls_verify, 1, errbuf);
    if (!c) return;
    curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
    curl_easy_setopt(c, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode rc = curl_easy_perform(c);
    cert_note(url, rc, errbuf, 0);
    curl_slist_free_all(hdrs);
    handle_give(c);
}

static void cert_note(const char *url, CURLcode rc, const char *errbuf, int may_probe) {
    const char *alert = rc == CURLE_OK ? NULL : cert_alert(errbuf);
    if (rc != CURLE_OK && !alert) {
        /* The connection broke without a word: perhaps a TLS 1.3 alert lost. */
        if (may_probe && !strncmp(url, "https://", 8) &&
            (rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR || rc == CURLE_SSL_CONNECT_ERROR))
            cert_probe(url);
        return;
    }
    char o[200];
    url_origin(url, o, sizeof o);
    pthread_mutex_lock(&g_cc_lock);
    char was[sizeof g_cc_refused];
    str_copy(was, sizeof was, g_cc_refused);
    if (!alert && strcmp(o, g_cc_refused_origin) != 0) {
        pthread_mutex_unlock(&g_cc_lock);
        return; /* another server's answer says nothing about this one */
    }
    str_copy(g_cc_refused_origin, sizeof g_cc_refused_origin, alert ? o : "");
    if (!alert) g_cc_refused[0] = 0;
    else if (g_cc_cert)
        snprintf(g_cc_refused, sizeof g_cc_refused, "%s didn't accept this console's client certificate (%s)", o, alert);
    else
        snprintf(g_cc_refused, sizeof g_cc_refused,
                 "%s asks for a client certificate (mTLS): add one under Client certificate in the web UI", o);
    int changed = strcmp(was, g_cc_refused) != 0;
    if (changed && g_cc_refused[0]) LOGW("%s", g_cc_refused);
    else if (changed) LOGI("%s accepts the connection again", o);
    pthread_mutex_unlock(&g_cc_lock);
}

/* url's server didn't answer at all: its refusal, if any, is forgotten (quietly). */
static void cert_forget(const char *url) {
    char o[200];
    url_origin(url, o, sizeof o);
    pthread_mutex_lock(&g_cc_lock);
    if (!strcmp(o, g_cc_refused_origin)) g_cc_refused[0] = g_cc_refused_origin[0] = 0;
    pthread_mutex_unlock(&g_cc_lock);
}

int http_client_cert_refused(const char *url, char *msg, size_t n) {
    char o[200] = "";
    if (url) url_origin(url, o, sizeof o);
    pthread_mutex_lock(&g_cc_lock);
    int r = g_cc_refused[0] != 0 && (!url || !strcmp(o, g_cc_refused_origin));
    if (r && msg) str_copy(msg, n, g_cc_refused);
    pthread_mutex_unlock(&g_cc_lock);
    return r;
}

int http_client_cert_info(char *info, size_t n) {
    pthread_mutex_lock(&g_cc_lock);
    int r = g_cc_cert != NULL;
    if (info) str_copy(info, n, r ? g_cc_info : "");
    pthread_mutex_unlock(&g_cc_lock);
    return r;
}

static void secret_free(char *p, size_t len) {
    if (!p) return;
    volatile char *v = p;
    while (len--) *v++ = 0;
    free(p);
}

#ifdef HAVE_OPENSSL
static char *bio_take(BIO *b, size_t *len) {
    char *data = NULL;
    long n = BIO_get_mem_data(b, &data);
    char *out = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (out) {
        memcpy(out, data, (size_t)n);
        out[n] = 0;
        *len = (size_t)n;
    }
    return out;
}

/* OpenSSL's words for its last error, or "" */
static void ssl_error(char *out, size_t n) {
    const char *why = ERR_reason_error_string(ERR_peek_last_error());
    str_copy(out, n, why ? why : "");
}

static int p12_parse(const void *data, size_t len, const char *password, EVP_PKEY **key, X509 **cert,
                     STACK_OF(X509) **chain, char *err, size_t en) {
    ERR_clear_error();
    BIO *b = len <= 0x7fffffff ? BIO_new_mem_buf(data, (int)len) : NULL;
    PKCS12 *p12 = b ? d2i_PKCS12_bio(b, NULL) : NULL;
    BIO_free(b);
    if (!p12) {
        snprintf(err, en, "not a PKCS#12 (.p12 / .pfx) file");
        return -1;
    }
    const char *pw = password ? password : "";
    if (PKCS12_mac_present(p12) && !PKCS12_verify_mac(p12, pw, -1) && !(!pw[0] && PKCS12_verify_mac(p12, NULL, 0))) {
        PKCS12_free(p12);
        snprintf(err, en, "wrong password for the certificate file");
        return -1;
    }
    int ok = PKCS12_parse(p12, pw, key, cert, chain);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    /* Older exports (RC2) need OpenSSL 3's legacy provider, where it can be had (not on the PS5). */
    if (!ok && OSSL_PROVIDER_try_load(NULL, "legacy", 1)) {
        ERR_clear_error();
        ok = PKCS12_parse(p12, pw, key, cert, chain);
    }
#endif
    PKCS12_free(p12);
    if (!ok || !*cert || !*key) {
        char why[160];
        ssl_error(why, sizeof why);
        snprintf(err, en,
                 "can't read the certificate file%s%s. Files exported with old encryption (RC2) aren't supported: "
                 "export it again with AES (see the README, Client certificate)",
                 why[0] ? ": " : "", why);
        if (*cert) X509_free(*cert);
        if (*key) EVP_PKEY_free(*key);
        sk_X509_pop_free(*chain, X509_free);
        *cert = NULL, *key = NULL, *chain = NULL;
        return -1;
    }
    if (!X509_check_private_key(*cert, *key)) {
        snprintf(err, en, "the key in the certificate file doesn't belong to its certificate");
        X509_free(*cert);
        EVP_PKEY_free(*key);
        sk_X509_pop_free(*chain, X509_free);
        *cert = NULL, *key = NULL, *chain = NULL;
        return -1;
    }
    return 0;
}

/* "CN=name, valid until 2027-05-01" ("expired 2027-05-01" if it is,
 * "not valid before 2026-01-01" if it isn't yet) */
static void describe_cert(X509 *cert, char *out, size_t n) {
    char cn[96] = "";
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof cn);
    struct tm tm;
    char until[32] = "?";
    const ASN1_TIME *na = X509_get0_notAfter(cert);
    memset(&tm, 0, sizeof tm);
    if (ASN1_TIME_to_tm(na, &tm)) strftime(until, sizeof until, "%Y-%m-%d", &tm);
    const ASN1_TIME *nb = X509_get0_notBefore(cert);
    if (X509_cmp_current_time(nb) > 0) {
        memset(&tm, 0, sizeof tm);
        if (ASN1_TIME_to_tm(nb, &tm)) strftime(until, sizeof until, "%Y-%m-%d", &tm);
        snprintf(out, n, "CN=%s, not valid before %s", cn[0] ? cn : "(none)", until);
        return;
    }
    int expired = X509_cmp_current_time(na) < 0;
    snprintf(out, n, "CN=%s, %s %s", cn[0] ? cn : "(none)", expired ? "expired" : "valid until", until);
}
#endif

int http_client_cert_load(const void *p12, size_t len, const char *password, int use, char *info, size_t in,
                          char *err, size_t en) {
    if (info && in) info[0] = 0;
    if (!p12) {
        if (!use) return 0;
        pthread_mutex_lock(&g_cc_lock);
        secret_free(g_cc_key, g_cc_key_len);
        free(g_cc_cert);
        g_cc_cert = g_cc_key = NULL;
        g_cc_cert_len = g_cc_key_len = 0;
        g_cc_info[0] = 0;
        g_cc_refused[0] = g_cc_refused_origin[0] = 0;
        g_probe_origin[0] = 0;
        pthread_mutex_unlock(&g_cc_lock);
        return 0;
    }
#ifdef HAVE_OPENSSL
    EVP_PKEY *key = NULL;
    X509 *cert = NULL;
    STACK_OF(X509) *chain = NULL;
    if (p12_parse(p12, len, password, &key, &cert, &chain, err, en)) return -1;
    char desc[sizeof g_cc_info];
    describe_cert(cert, desc, sizeof desc);
    if (info) str_copy(info, in, desc);
    int rc = 0;
    if (use) {
        BIO *cb = BIO_new(BIO_s_mem()), *kb = BIO_new(BIO_s_mem());
        int ok = cb && kb && PEM_write_bio_X509(cb, cert);
        for (int i = 0; ok && i < sk_X509_num(chain); i++) ok = PEM_write_bio_X509(cb, sk_X509_value(chain, i));
        ok = ok && PEM_write_bio_PrivateKey(kb, key, NULL, NULL, 0, NULL, NULL);
        size_t cl = 0, kl = 0;
        char *c = ok ? bio_take(cb, &cl) : NULL, *k = ok ? bio_take(kb, &kl) : NULL;
        BIO_free(cb);
        BIO_free(kb); /* OpenSSL clears a memory BIO's buffer as it frees it */
        if (!c || !k) {
            free(c);
            secret_free(k, kl);
            snprintf(err, en, "out of memory reading the certificate");
            rc = -1;
        } else {
            pthread_mutex_lock(&g_cc_lock);
            secret_free(g_cc_key, g_cc_key_len);
            free(g_cc_cert);
            g_cc_cert = c, g_cc_cert_len = cl;
            g_cc_key = k, g_cc_key_len = kl;
            str_copy(g_cc_info, sizeof g_cc_info, desc);
            g_cc_refused[0] = g_cc_refused_origin[0] = 0;
            g_probe_origin[0] = 0;
            pthread_mutex_unlock(&g_cc_lock);
        }
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    sk_X509_pop_free(chain, X509_free);
    return rc;
#else
    (void)len, (void)password, (void)use;
    snprintf(err, en, "this build can't read client certificates (no OpenSSL)");
    return -1;
#endif
}
