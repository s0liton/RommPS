/* For tests/gameid_test.sh: a disc's PlayStation serial and PS2 CRC.
 *   gameid_cli <disc>        prints "<serial> <crc>", "-" for what it can't read
 *   gameid_cli xbox <game>   prints the Xbox title id, or "-"
 *   gameid_cli x360 <game>   prints the Xbox 360 title id, or "-"
 *   gameid_cli xbox-short <game>   prints "<needs> <has>" for an image cut short, or "-" */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gameid.h"

/* state.c's, for gameid_from_library, which this doesn't call. */
const char *jget_str(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

int main(int argc, char **argv) {
    char serial[32];
    if (argc == 3 && !strcmp(argv[1], "xbox-short")) {
        uint64_t need = 0, have = 0;
        if (gameid_xbox_short(argv[2], &need, &have) == 1) printf("%llu %llu\n", (unsigned long long)need, (unsigned long long)have);
        else printf("-\n");
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "xbox") || !strcmp(argv[1], "x360"))) {
        const int rc = !strcmp(argv[1], "xbox") ? gameid_xbox(argv[2], serial, sizeof serial) : gameid_x360(argv[2], serial, sizeof serial);
        printf("%s\n", rc == 0 ? serial : "-");
        return 0;
    }
    if (argc != 2) return 2;
    uint32_t crc;
    int s = gameid_psx(argv[1], serial, sizeof serial), c = gameid_ps2_crc(argv[1], &crc);
    printf("%s ", s == 0 ? serial : "-");
    if (c == 0) printf("%08X\n", crc);
    else printf("-\n");
    return 0;
}
