/* Zip files: writing uses stored entries, reading also handles deflate. */
#ifndef ROMM_ZIP_H
#define ROMM_ZIP_H

#include <stddef.h>

typedef struct {
    const char *path; /* file on disk */
    const char *name; /* entry name inside the zip, "/" separated */
} zip_member;

int zip_write(const char *zip_path, const zip_member *members, int count);

/* Writes a zip holding each file under its base name. */
int zip_store_files(const char *zip_path, const char *const *files, int count);

/* Extracts the entry called `name`. */
int zip_extract_entry(const char *zip_path, const char *name, const char *dest);

/* Extracts every file entry under dest_dir. Entry names that would leave
 * dest_dir are refused. Returns the number of files, or -1. */
int zip_extract_all(const char *zip_path, const char *dest_dir);

/* RomM's content hash for a zip: md5 of the sorted "name:md5" lines. */
int zip_content_hash(const char *zip_path, char out[33]);

#endif
