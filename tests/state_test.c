/* The state's index (state_index, for the scan's lookups of every game): it
 * has to answer exactly as state_entry() does, whatever happened to the state
 * since it was built. Run under the address sanitizer, so a record it kept
 * after the state let go of it shows.
 *   make build/host/state_test && build/host/state_test */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "state.h"

static int failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);              \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static char g_dir[] = "/tmp/romm-state-test-XXXXXX";
const char *plat_data_dir(void) { return g_dir; }

#define GAMES 3000
static void key(char *out, size_t n, int i) { snprintf(out, n, "/games/snes/Game %d (USA).sfc", i); }

/* Another thread dropping a record its own way, under the lock, as the
 * library does when a game is deleted. */
static void *drop(void *arg) {
    state_lock();
    cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), arg);
    state_unlock();
    return NULL;
}

int main(void) {
    char k[128], to[128];
    if (!mkdtemp(g_dir)) return 1;
    state_load();
    state_lock();
    for (int i = 0; i < GAMES; i++) {
        key(k, sizeof k, i);
        cJSON_AddNumberToObject(state_entry_ensure("roms", k), "rom_id", i + 1);
    }
    state_index *ix = state_index_new("roms");
    CHECK(ix != NULL);
    if (!ix) return 1;

    /* Every record, the same one state_entry() finds; nothing for a stranger. */
    int same = 1;
    for (int i = 0; i < GAMES; i++) {
        key(k, sizeof k, i);
        same &= state_index_get(ix, k) == state_entry("roms", k) && state_entry("roms", k) != NULL;
    }
    CHECK(same);
    CHECK(state_index_get(ix, "/games/snes/Nobody.sfc") == NULL);
    CHECK(state_index_get(ix, "") == NULL);

    /* A record made after the index was built. */
    cJSON *late = state_entry_ensure("roms", "/games/snes/Late Arrival.sfc");
    CHECK(state_index_get(ix, "/games/snes/Late Arrival.sfc") == late);

    /* One removed: gone from the index too, its neighbours still there. */
    key(k, sizeof k, 7);
    state_entry_remove("roms", k);
    CHECK(state_index_get(ix, k) == NULL);
    key(k, sizeof k, 8);
    CHECK(state_index_get(ix, k) == state_entry("roms", k) && state_entry("roms", k) != NULL);

    /* A folder that moved: its records answer at the new place only. */
    for (int i = 0; i < 3; i++) {
        snprintf(k, sizeof k, "/old/psx/Disc %d.chd", i);
        cJSON_AddNumberToObject(state_entry_ensure("roms", k), "rom_id", 9000 + i);
        CHECK(state_index_get(ix, k) != NULL);
    }
    CHECK(state_rekey("/old/psx", "/new/psx") == 3);
    CHECK(state_index_get(ix, "/old/psx/Disc 1.chd") == NULL);
    CHECK(state_index_get(ix, "/new/psx/Disc 1.chd") == state_entry("roms", "/new/psx/Disc 1.chd"));
    CHECK((int)cJSON_GetNumberValue(cJSON_GetObjectItem(state_index_get(ix, "/new/psx/Disc 2.chd"), "rom_id")) == 9002);

    /* Another thread removes a record without going through state.c: taking
     * the lock is what tells the index, so it mustn't hand out what's freed. */
    key(k, sizeof k, 100);
    CHECK(state_index_get(ix, k) != NULL);
    state_unlock();
    pthread_t t;
    pthread_create(&t, NULL, drop, k);
    pthread_join(t, NULL);
    state_lock();
    CHECK(state_index_get(ix, k) == NULL);
    key(to, sizeof to, 101);
    CHECK(state_index_get(ix, to) == state_entry("roms", to) && state_entry("roms", to) != NULL);

    /* The state read again from disk: every record is a new one. */
    CHECK(state_save() == 0);
    state_load();
    CHECK(state_index_get(ix, to) == state_entry("roms", to) && state_entry("roms", to) != NULL);
    CHECK(state_index_get(ix, k) == NULL);

    /* Another section doesn't answer for this one. */
    cJSON_AddNumberToObject(state_entry_ensure("saves", "/saves/Only Here.srm"), "id", 1);
    CHECK(state_index_get(ix, "/saves/Only Here.srm") == NULL);

    state_index_free(ix);
    state_index_free(NULL);
    state_unlock();
    char path[160];
    snprintf(path, sizeof path, "%s/state.json", g_dir);
    remove(path);
    remove(g_dir);
    if (failures) fprintf(stderr, "%d failed\n", failures);
    else printf("state_test: ok\n");
    return failures != 0;
}
