#include "version.h"

#include <stdlib.h>
#include <string.h>

/* "v1.2.3-dev.4+build" into its numbers (missing ones 0) and how many there
 * were; returns the pre-release part, "dev.4+build", or "" for a release. */
static const char *parse(const char *s, int v[3], int *count) {
    v[0] = v[1] = v[2] = 0;
    *count = 0;
    if (!s) return "";
    if (*s == 'v') s++;
    for (int i = 0; i < 3; i++) {
        char *end;
        long n = strtol(s, &end, 10);
        if (end == s) return "";
        v[i] = (int)n;
        ++*count;
        s = end;
        if (i < 2) {
            if (*s != '.') break;
            s++;
        }
    }
    return *s == '-' ? s + 1 : "";
}

static int digits_only(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return n > 0;
}

static int pre_cmp(const char *a, const char *b) {
    int ra = !*a || *a == '+', rb = !*b || *b == '+';
    if (ra || rb) return ra - rb; /* the release wins */
    for (;;) {
        size_t la = strcspn(a, ".+"), lb = strcspn(b, ".+");
        int na = digits_only(a, la), nb = digits_only(b, lb), c;
        if (na && nb) {
            /* Any length, no strtol overflow: past the leading zeros, longer is bigger. */
            while (la > 1 && *a == '0') a++, la--;
            while (lb > 1 && *b == '0') b++, lb--;
            c = la != lb ? (la < lb ? -1 : 1) : memcmp(a, b, la);
        } else if (na != nb) {
            c = na ? -1 : 1; /* numbers before words */
        } else {
            c = memcmp(a, b, la < lb ? la : lb);
            if (!c) c = (la > lb) - (la < lb);
        }
        if (c) return c < 0 ? -1 : 1;
        a += la, b += lb;
        int ea = *a != '.', eb = *b != '.';
        if (ea || eb) return eb - ea; /* fewer parts is older */
        a++, b++;
    }
}

int version_cmp(const char *a, const char *b) {
    int x[3], y[3], n;
    const char *pa = parse(a, x, &n), *pb = parse(b, y, &n);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return pre_cmp(pa, pb);
}

int version_valid(const char *s) {
    int v[3], n;
    parse(s, v, &n);
    return n >= 2;
}
