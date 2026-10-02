/* libcurl wrapper. Each call uses its own handle, so it is thread-safe. */
#ifndef ROMM_HTTP_H
#define ROMM_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    long status;   /* HTTP status, 0 on transport error */
    char *body;    /* malloc'd, NUL-terminated (may be NULL) */
    size_t len;
    char error[256];
} http_resp;

/* Progress callback: return non-zero to abort. total may be 0 if unknown. */
typedef int (*http_progress_fn)(void *ud, int64_t done, int64_t total);

void http_global_init(const char *ca_bundle, int tls_verify);
void http_set_tls_verify(int verify);
void http_resp_free(http_resp *r);

/* auth may be NULL or a full header value ("Bearer rmm_..."). */
int http_request(const char *method, const char *url, const char *auth, const char *content_type,
                 const void *body, size_t body_len, http_resp *out);

/* multipart/form-data upload of one file. */
int http_upload_file(const char *method, const char *url, const char *auth, const char *field,
                     const char *file_path, const char *upload_name, http_resp *out);

/* Downloads to dest.part, resuming with Range if asked, then renames it to dest. */
int http_download(const char *url, const char *auth, const char *dest, int resume,
                  http_progress_fn progress, void *ud, http_resp *out);

/* URL-encode a query/path component into a malloc'd string. */
char *http_escape(const char *s);

#endif
