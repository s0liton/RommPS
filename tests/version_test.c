/* Which version wins: releases, dev builds between them, and the strings
 * older versions left lying around (make unit).
 *   cc -Isrc tests/version_test.c src/version.c -o build/host/version_test */
#include <stdio.h>

#include "version.h"

static int failures;

/* want is -1, 0 or 1; b against a is checked too, as it must be the opposite. */
static void check(const char *a, const char *b, int want) {
    int got = version_cmp(a, b), back = version_cmp(b, a);
    if (got != want || back != -want) {
        fprintf(stderr, "version_cmp(\"%s\", \"%s\") = %d (and %d the other way), want %d\n", a, b, got, back, want);
        failures++;
    }
}

static void valid(const char *s, int want) {
    if (version_valid(s) != want) {
        fprintf(stderr, "version_valid(\"%s\") = %d, want %d\n", s, !want, want);
        failures++;
    }
}

int main(void) {
    /* Releases, as ever. */
    check("1.4.2", "1.4.2", 0);
    check("1.4.2", "1.4.3", -1);
    check("1.4.10", "1.4.9", 1);
    check("1.10.0", "1.9.9", 1);
    check("2.0.0", "1.99.99", 1);
    check("v1.4.2", "1.4.2", 0);
    check("1.4", "1.4.0", 0);
    check("1.4", "1.4.1", -1);

    /* Dev builds: after the release they follow, before the one they lead to. */
    check("1.4.3-dev.1", "1.4.2", 1);
    check("1.4.3-dev.7", "1.4.3-dev.8", -1);
    check("1.4.3-dev.9", "1.4.3-dev.10", -1); /* numbers as numbers */
    check("1.4.3-dev.12", "1.4.3-dev.12", 0);
    check("1.4.3-dev.99", "1.4.3", -1);
    check("1.4.3-dev.99", "1.4.4", -1);
    check("1.4.3-dev.99", "1.5.0", -1);
    check("v1.4.3-dev.3", "1.4.3-dev.3", 0);
    check("1.4.3-dev.123456789012345678901", "1.4.3-dev.99", 1); /* no overflow */
    check("1.4.3-dev.007", "1.4.3-dev.7", 0);

    /* The rest of semver's order, for whatever comes later. */
    check("1.0.0-alpha", "1.0.0-alpha.1", -1);
    check("1.0.0-alpha.1", "1.0.0-alpha.beta", -1);
    check("1.0.0-alpha.beta", "1.0.0-beta", -1);
    check("1.0.0-beta", "1.0.0-beta.2", -1);
    check("1.0.0-beta.2", "1.0.0-beta.11", -1);
    check("1.0.0-beta.11", "1.0.0-rc.1", -1);
    check("1.0.0-rc.1", "1.0.0", -1);
    check("1.0.0+build.5", "1.0.0", 0);
    check("1.0.0-dev.2+abc", "1.0.0-dev.2", 0);
    check("1.0.0-dev.2+abc", "1.0.0", -1);
    check("1.2.3-ps4-prototype.6", "1.2.3-ps4-prototype.7", -1);

    /* What isn't a version counts as 0.0.0, as it did. */
    check("", "0.0.0", 0);
    check("dev", "0.0.0", 0);
    check("", "1.1.0", -1);
    check("dev", "1.4.3-dev.1", -1);
    check("1.1.0", "", 1);
    if (version_cmp(NULL, "0.0.0") != 0) {
        fprintf(stderr, "version_cmp(NULL, \"0.0.0\") != 0\n");
        failures++;
    }

    valid("1.4.2", 1);
    valid("v1.4.2", 1);
    valid("1.4", 1);
    valid("1.4.3-dev.12", 1);
    valid("1", 0);
    valid("dev", 0);
    valid("", 0);
    valid(NULL, 0);

    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    puts("version: ok");
    return 0;
}
