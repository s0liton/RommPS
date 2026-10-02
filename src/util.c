#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "md5.h"


#define LOG_RING_LINES 200
#define LOG_LINE_LEN   320
#define LOG_FILE_MAX   (512 * 1024)

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log_ring[LOG_RING_LINES][LOG_LINE_LEN];
static int g_log_head, g_log_count;
static char g_log_path[PATH_MAX_LEN];

int thread_start(void *(*fn)(void *), void *arg) {
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1024 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return rc;
}

void log_init(const char *file_path) {
    str_copy(g_log_path, sizeof g_log_path, file_path);
    /* Keep one old log. */
    if (file_size(g_log_path) > LOG_FILE_MAX) {
        char old[PATH_MAX_LEN];
        snprintf(old, sizeof old, "%s.1", g_log_path);
        rename(g_log_path, old);
    }
}

void log_msg(int level, const char *fmt, ...) {
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char msg[LOG_LINE_LEN - 32], line[LOG_LINE_LEN], ts[32];
    time_t now = time(NULL);
    struct tm tm;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(line, sizeof line, "%s [%s] %s", ts, names[level & 3], msg);

    pthread_mutex_lock(&g_log_lock);
    fprintf(stderr, "%s\n", line);
    if (level >= LOG_INFO) {
        str_copy(g_log_ring[g_log_head], LOG_LINE_LEN, line);
        g_log_head = (g_log_head + 1) % LOG_RING_LINES;
        if (g_log_count < LOG_RING_LINES) g_log_count++;
    }
    if (g_log_path[0]) {
        FILE *f = fopen(g_log_path, "a");
        if (f) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    }
    pthread_mutex_unlock(&g_log_lock);
}

char *log_recent(void) {
    pthread_mutex_lock(&g_log_lock);
    size_t cap = (size_t)g_log_count * LOG_LINE_LEN + 1, len = 0;
    char *out = malloc(cap);
    if (out) {
        out[0] = 0;
        int start = (g_log_head - g_log_count + LOG_RING_LINES) % LOG_RING_LINES;
        for (int i = 0; i < g_log_count; i++) {
            const char *l = g_log_ring[(start + i) % LOG_RING_LINES];
            len += (size_t)snprintf(out + len, cap - len, "%s\n", l);
        }
    }
    pthread_mutex_unlock(&g_log_lock);
    return out;
}


int mkdir_p(const char *path) {
    char tmp[PATH_MAX_LEN];
    str_copy(tmp, sizeof tmp, path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

int mkdir_parent(const char *file_path) {
    char dir[PATH_MAX_LEN];
    str_copy(dir, sizeof dir, file_path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return 0;
    *slash = 0;
    return mkdir_p(dir);
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

time_t file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

int64_t file_mtime_ns(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#ifdef __APPLE__
    return (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

int set_file_mtime(const char *path, time_t t) {
    struct timeval tv[2] = {{t, 0}, {t, 0}};
    return utimes(path, tv);
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (!buf) return NULL;
    buf[sz] = 0;
    if (len) *len = (size_t)sz;
    return buf;
}

int write_file_atomic(const char *path, const void *data, size_t len) {
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdir_parent(path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, path);
}

int copy_file(const char *src, const char *dst) {
    char tmp[PATH_MAX_LEN];
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    char *buf = malloc(64 * 1024);
    if (!buf) {
        fclose(in);
        return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    mkdir_parent(dst);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        free(buf);
        return -1;
    }
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, 64 * 1024, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    fclose(in);
    free(buf);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    if (rc == 0) rc = rename(tmp, dst);
    if (rc != 0) unlink(tmp);
    return rc;
}

int move_file(const char *src, const char *dst) {
    mkdir_parent(dst);
    if (rename(src, dst) == 0) return 0;
    /* Across drives, e.g. /data to /mnt/usb0. */
    if (copy_file(src, dst) != 0) return -1;
    unlink(src);
    return 0;
}

void remove_tree(const char *path) {
    if (!dir_exists(path)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[PATH_MAX_LEN];
        path_join(p, sizeof p, path, de->d_name);
        remove_tree(p);
    }
    if (d) closedir(d);
    rmdir(path);
}

const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void path_join(char *out, size_t n, const char *a, const char *b) {
    size_t la = strlen(a);
    if (la && a[la - 1] == '/')
        snprintf(out, n, "%s%s", a, b);
    else
        snprintf(out, n, "%s/%s", a, b);
}

void path_stem(char *out, size_t n, const char *file_name) {
    str_copy(out, n, path_basename(file_name));
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

const char *path_ext(const char *file_name) {
    const char *base = path_basename(file_name);
    const char *dot = strrchr(base, '.');
    return (dot && dot != base) ? dot : "";
}


int str_ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

int str_ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcasecmp(s + ls - lx, suffix) == 0;
}

int ext_in_list(const char *file_name, const char *list) {
    if (!list) return 0;
    if (!strcmp(list, "*")) return 1; /* any file */
    const char *ext = path_ext(file_name);
    if (!*ext) return 0;
    char buf[512];
    str_copy(buf, sizeof buf, list);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        size_t lt = strlen(tok);
        if (lt && tok[lt - 1] == '*') {
            if (strncasecmp(ext, tok, lt - 1) == 0) return 1;
        } else if (strcasecmp(ext, tok) == 0) {
            return 1;
        }
    }
    return 0;
}

void str_copy(char *dst, size_t n, const char *src) {
    if (!n) return;
    if (!src) src = "";
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

void str_template(char *out, size_t n, const char *tpl, const char *const *vars) {
    size_t o = 0;
    for (const char *p = tpl; *p && o + 1 < n;) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            int matched = 0;
            if (end) {
                size_t kl = (size_t)(end - p - 1);
                for (int i = 0; vars[i]; i += 2) {
                    if (strlen(vars[i]) == kl && strncmp(p + 1, vars[i], kl) == 0) {
                        const char *v = vars[i + 1] ? vars[i + 1] : "";
                        size_t vl = strlen(v);
                        if (o + vl >= n) vl = n - 1 - o;
                        memcpy(out + o, v, vl);
                        o += vl;
                        p = end + 1;
                        matched = 1;
                        break;
                    }
                }
            }
            if (matched) continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}


void iso8601_utc(time_t t, char *out, size_t n) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%S+00:00", &tm);
}

/* timegm isn't everywhere, so here's yet another copy of it. */
static time_t utc_mktime(struct tm *tm) {
    static const int mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int y = tm->tm_year + 1900, m = tm->tm_mon;
    long days = (y - 1970) * 365L + ((y - 1969) / 4) - ((y - 1901) / 100) + ((y - 1601) / 400);
    days += mdays[m] + tm->tm_mday - 1;
    if (m > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return (time_t)(((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 + tm->tm_sec);
}

time_t iso8601_parse(const char *s) {
    if (!s) return 0;
    struct tm tm = {0};
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    time_t t = utc_mktime(&tm);
    /* Z, +HH:MM or -HH:MM, possibly after fractional seconds. */
    const char *tz = strchr(s, 'T');
    if (tz) {
        tz++;
        while (*tz && (*tz == ':' || *tz == '.' || (*tz >= '0' && *tz <= '9'))) tz++;
        int th, tmin = 0;
        if ((*tz == '+' || *tz == '-') && sscanf(tz + 1, "%d:%d", &th, &tmin) >= 1) {
            long off = th * 3600L + tmin * 60L;
            t = (*tz == '+') ? t - off : t + off;
        }
    }
    return t;
}


static void hex_digest(const unsigned char d[16], char out[33]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 15];
    }
    out[32] = 0;
}

int md5_file_hex(const char *path, char out[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    romm_md5_CTX ctx;
    unsigned char d[16], *buf = malloc(32 * 1024);
    size_t n;
    if (!buf) {
        fclose(f);
        return -1;
    }
    romm_md5_Init(&ctx);
    while ((n = fread(buf, 1, 32 * 1024, f)) > 0) romm_md5_Update(&ctx, buf, (unsigned long)n);
    free(buf);
    fclose(f);
    romm_md5_Final(d, &ctx);
    hex_digest(d, out);
    return 0;
}

void md5_hex(const void *data, size_t len, char out[33]) {
    romm_md5_CTX ctx;
    unsigned char d[16];
    romm_md5_Init(&ctx);
    romm_md5_Update(&ctx, data, (unsigned long)len);
    romm_md5_Final(d, &ctx);
    hex_digest(d, out);
}

void random_uuid(char out[37]) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)rand();
    }
    if (fd >= 0) close(fd);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
}
