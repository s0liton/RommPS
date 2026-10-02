/* state.json: local games and their RomM ids, what was last synced, and open
 * conflicts. Hold state_lock() while using it. */
#ifndef ROMM_STATE_H
#define ROMM_STATE_H

#include "cJSON.h"

/* Sections, keyed by local path (bundles use "<folder>#<game id>"):
 *   roms       rom_id, name, profile, platform_dir, stems, fs_name, md5, game_id
 *   saves      rom_id, slot, hash, mtime, mtime_ns, size, save_id, synced_at
 *   states     rom_id, hash, mtime, mtime_ns, size, state_id, server_updated_at, synced_at
 *   memcards   card_id, hash, version_id, backed_up_at
 *   conflicts  rom_id, save_id, slot, reason, server_updated_at, server_hash, local_mtime
 *   history    recent sync summaries, newest last */
int state_load(void);
int state_save(void);
void state_lock(void);
void state_unlock(void);

cJSON *state_section(const char *name);           /* object, created on demand */
cJSON *state_entry(const char *section, const char *key); /* NULL if missing */
cJSON *state_entry_ensure(const char *section, const char *key);
void state_entry_remove(const char *section, const char *key);

/* Get and set values on an entry. */
void jset_num(cJSON *o, const char *k, double v);
void jset_str(cJSON *o, const char *k, const char *v);
double jget_num(const cJSON *o, const char *k, double def);
const char *jget_str(const cJSON *o, const char *k, const char *def);

void state_history_add(cJSON *summary); /* takes ownership, keeps last 30 */

#endif
