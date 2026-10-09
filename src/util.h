/* Small portable helpers: logging, files, paths, time, hashing. */
#ifndef ROMM_UTIL_H
#define ROMM_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define APP_NAME    "romm-sync"
#ifndef APP_VERSION
#define APP_VERSION "1.1.1"
#endif
#ifndef APP_BUILD /* git describe of the build, from the Makefile */
#define APP_BUILD "unknown"
#endif

#define PATH_MAX_LEN 1024

/* Starts a detached thread with a 1 MB stack. PS5 threads get a tiny stack by
 * default, which I found out the fun way: zipping a PSP save took the whole
 * payload down. 0 on success. */
int thread_start(void *(*fn)(void *), void *arg);

/* Logging goes to stderr, the log file and a buffer the web UI reads. */
enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };
void log_init(const char *file_path);
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Recent log lines, newline separated. Free the result. */
char *log_recent(void);

#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)

int mkdir_p(const char *path);                  /* 0 ok */
int mkdir_parent(const char *file_path);        /* mkdir -p dirname(file_path) */
int file_exists(const char *path);
int dir_exists(const char *path);
int64_t file_size(const char *path);            /* -1 if missing */
time_t file_mtime(const char *path);            /* 0 if missing */
int64_t file_mtime_ns(const char *path);        /* nanoseconds, 0 if missing */
int set_file_mtime(const char *path, time_t t);
char *read_file(const char *path, size_t *len); /* malloc'd, NUL-terminated */
int write_file_atomic(const char *path, const void *data, size_t len);
int copy_file(const char *src, const char *dst);

/* -1, 0 or 1 as a is older, the same as or newer than b ("1.2.3", a leading
 * "v" and anything after the patch ignored). Unreadable counts as 0.0.0. */
int version_cmp(const char *a, const char *b);
/* The version a payload ELF says it is (its ROMM_SYNC_VERSION= tag, from
 * 1.1.0 on), or "" if it has none. */
void payload_version(const void *elf, size_t len, char *out, size_t n);
int link_or_copy(const char *src, const char *dst); /* hard link, a copy across drives; folders too */
int move_file(const char *src, const char *dst); /* rename, falls back to copy+unlink */
void remove_tree(const char *path);              /* rm -rf */
const char *path_basename(const char *path);
void path_join(char *out, size_t n, const char *a, const char *b);
/* "Foo (USA).sfc" -> "Foo (USA)" */
void path_stem(char *out, size_t n, const char *file_name);
const char *path_ext(const char *file_name);    /* ".sfc" or "" */

int str_ieq(const char *a, const char *b);
int str_ends_with_ci(const char *s, const char *suffix);
/* Is the file's extension in a list like ".srm,.sav,.state*"? */
int ext_in_list(const char *file_name, const char *list);
void str_copy(char *dst, size_t n, const char *src);
/* Fills {key} placeholders. vars holds key, value pairs and ends with NULL. */
void str_template(char *out, size_t n, const char *tpl, const char *const *vars);

void iso8601_utc(time_t t, char *out, size_t n);  /* 2026-09-29T15:26:00+00:00 */
time_t iso8601_parse(const char *s);             /* 0 on failure */

int md5_file_hex(const char *path, char out[33]);
/* FNV-1a 64 of a file, 16 lower-case hex digits (PS5N64 names saves by it). */
int fnv1a64_file_hex(const char *path, char out[17]);
void md5_hex(const void *data, size_t len, char out[33]);
void random_uuid(char out[37]);

#endif
