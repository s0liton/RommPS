/* libcurl wrapper, thread-safe. Handles are kept between requests, and with
 * them their open connections, so requests to the same server reuse them. */
#ifndef ROMM_HTTP_H
#define ROMM_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    long status;   /* HTTP status, 0 on transport error */
    char *body;    /* malloc'd, NUL-terminated (may be NULL) */
    size_t len;
    char error[256];
    int no_answer; /* no connection at all: refused, unknown host or timed out */
    int aborted;   /* the progress callback stopped it */
} http_resp;

/* Progress callback: return non-zero to abort. total may be 0 if unknown. */
typedef int (*http_progress_fn)(void *ud, int64_t done, int64_t total);

void http_global_init(const char *ca_bundle, int tls_verify);
void http_set_tls_verify(int verify);
void http_resp_free(http_resp *r);

/* auth may be NULL or a full header value ("Bearer rmm_..."). */
int http_request(const char *method, const char *url, const char *auth, const char *content_type,
                 const void *body, size_t body_len, http_resp *out);

/* A GET whose JSON reply drops the members named in drop (NULL-terminated)
 * as it arrives, so they never take memory: for big lists. */
int http_get_lean(const char *url, const char *auth, const char *const *drop, http_resp *out);

/* multipart/form-data upload of one file. */
int http_upload_file(const char *method, const char *url, const char *auth, const char *field,
                     const char *file_path, const char *upload_name, http_resp *out);

/* Downloads to dest.part, resuming with Range if asked, then renames it to dest. */
int http_download(const char *url, const char *auth, const char *dest, int resume,
                  http_progress_fn progress, void *ud, http_resp *out);

/* mTLS: the certificate and key in a PKCS#12 (.p12) file, shown to the RomM
 * server when it asks for one; never to the verified requests (GitHub).
 * use 0 only reads it. p12 NULL (with use) takes it away. info gets
 * "CN=name, valid until 2027-05-01". 0, or -1 and why in err. */
int http_client_cert_load(const void *p12, size_t len, const char *password, int use, char *info, size_t in,
                          char *err, size_t en);
/* 1, and info as above, while one is in use. */
int http_client_cert_info(char *info, size_t n);
/* 1, and why, while url's server (any, if NULL) turns the TLS handshake down
 * over the client certificate: it wants one, or won't take this one. */
int http_client_cert_refused(const char *url, char *msg, size_t n);

/* 1 if url's TLS certificate fails verification (self-signed, private CA). */
int http_tls_untrusted(const char *url);

/* For updates: always verify TLS certificates, whatever tls_verify says. */
int http_get_verified(const char *url, http_resp *out);
int http_download_verified(const char *url, const char *dest, http_progress_fn progress, void *ud,
                           http_resp *out);

/* 1 while url's server isn't answering: requests to it fail at once, and one
 * at a time checks whether it's back (every 5 to 30 s). */
int http_down(const char *url);

/* Called (on the thread that found out) when a server that wasn't answering
 * answers again, with its origin ("http://10.0.0.2:8080"). */
typedef void (*http_back_fn)(const char *origin);
void http_on_back(http_back_fn fn);

/* 1 if a request failed for want of memory in the last 30 s: smaller
 * requests or the same one again right away won't do better. */
int http_out_of_memory(void);

/* URL-encode a query/path component into a malloc'd string. */
char *http_escape(const char *s);

#endif
