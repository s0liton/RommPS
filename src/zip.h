/* Zip files: written deflated, read stored or deflated. */
#ifndef ROMM_ZIP_H
#define ROMM_ZIP_H

#include <stddef.h>
#include <time.h>

typedef struct {
    const char *path; /* file on disk */
    const char *name; /* entry name inside the zip, "/" separated */
} zip_member;

/* 0, -1, or -2 for files past what a zip without zip64 holds (4 GB). */
int zip_write(const char *zip_path, const zip_member *members, int count);

/* Writes a zip holding each file under its base name. */
int zip_store_files(const char *zip_path, const char *const *files, int count);

/* Extracts the entry called `name`. */
int zip_extract_entry(const char *zip_path, const char *name, const char *dest);

/* Extracts every file entry under dest_dir. Entry names that would leave
 * dest_dir are refused. Returns the number of files, or -1. */
int zip_extract_all(const char *zip_path, const char *dest_dir);

/* The shallowest entry named suffix or ending in "/" suffix (any case; the
 * fewest folders deep, then the shortest name), read without loading the
 * whole zip: a Vita dump's app/PCSE00001/sce_sys/param.sfo out of a 3 GB
 * .vpk. Zip64 too. A malloc'd copy in *out, its size in *len; -1 for none,
 * one bigger than max, or a zip it can't read. */
int zip_read_suffix(const char *zip_path, const char *suffix, size_t max, unsigned char **out, size_t *len);

/* When the zip's first entry was last written (its file's time when we made
 * the zip), or 0 if it doesn't say. */
time_t zip_first_time(const char *zip_path);

/* RomM's content hash for a zip: md5 of the sorted "name:md5" lines. */
int zip_content_hash(const char *zip_path, char out[33]);

#endif
