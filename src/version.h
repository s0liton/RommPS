/* Which version is newer. Releases are "1.4.2"; a dev build is the next patch
 * with a pre-release part, "1.4.3-dev.12", so it's newer than the release it
 * follows and older than the one that replaces it. Plain libc: RommPS
 * (platform/ps5/app) builds this file too, so the app and the payload never
 * disagree about which copy wins. */
#ifndef ROMM_VERSION_H
#define ROMM_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/* -1, 0 or 1 as a is older, the same as or newer than b, by semver's rules:
 * the three numbers first, then a release beats its pre-releases, which go dot
 * by dot, numbers as numbers ("dev.9" < "dev.10" < "dev.a"), and fewer parts
 * first ("dev" < "dev.1"). A leading "v" and a "+build" are ignored; a missing
 * number is 0, and anything that isn't a version counts as 0.0.0. */
int version_cmp(const char *a, const char *b);

/* At least "1.2", so a name like "dev" isn't taken for 0.0.0. */
int version_valid(const char *s);

#ifdef __cplusplus
}
#endif

#endif
