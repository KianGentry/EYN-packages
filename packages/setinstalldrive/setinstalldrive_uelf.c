#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Set install binary lookup drive.", "setinstalldrive 0");

static void usage(void) {
    puts("Usage: setinstalldrive <logical-drive>");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }

    char* end = NULL;
    unsigned long logical = strtoul(argv[1], &end, 10);
    if (!end || *end != '\0' || logical > 255UL) {
        puts("setinstalldrive: invalid drive number");
        return 1;
    }

    if (eyn_sys_drive_is_present((uint32_t)logical) <= 0) {
        puts("setinstalldrive: drive not present");
        return 1;
    }

    if (eyn_sys_systemcfg_set_install_drive((uint32_t)logical) != 0) {
        puts("setinstalldrive: failed to persist install drive");
        return 1;
    }

    printf("Install binary lookup drive set to %lu\n", logical);
    return 0;
}
