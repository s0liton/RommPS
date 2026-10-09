/* Throwaway compile+link check for platform/ps5/platform.c (not meant to run). */
#include <stdio.h>
#include "platform.h"

int main(void) {
    char tid[16], ip[64];
    if (plat_init()) return 1;
    plat_notify("romm-sync %s test", plat_name());
    printf("title=%d %s\n", plat_running_title(tid, sizeof tid), tid);
    plat_local_ip(ip, sizeof ip);
    printf("data=%s ca=%s ip=%s tile=%d\n", plat_data_dir(),
           plat_ca_bundle() ? plat_ca_bundle() : "(default)", ip, plat_install_tile(8780));
    return 0;
}
