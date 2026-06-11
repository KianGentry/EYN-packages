#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Set or list logical drive labels.", "drivelabel list");

#define LABEL_CAP 32

static void usage(void) {
    puts("Usage: drivelabel list");
    puts("       drivelabel set <logical-drive> <label>");
    puts("       drivelabel clear <logical-drive>");
}

static int parse_drive(const char* s, uint32_t* out) {
    char* end = NULL;
    unsigned long v;
    if (!s || !*s || !out) return -1;
    v = strtoul(s, &end, 10);
    if (!end || *end != '\0' || v > 255UL) return -1;
    *out = (uint32_t)v;
    return 0;
}

static void print_labels(void) {
    int count = eyn_sys_drive_get_count();
    int d;
    if (count <= 0) {
        puts("drivelabel: no drives available");
        return;
    }
    for (d = 0; d < count; ++d) {
        char label[LABEL_CAP];
        int rc;
        if (eyn_sys_drive_is_present((uint32_t)d) <= 0) continue;
        label[0] = '\0';
        rc = eyn_sys_systemcfg_get_drive_label((uint32_t)d, label, (int)sizeof(label));
        if (rc > 0 && label[0]) printf("%d => %s\n", d, label);
        else printf("%d => (unlabelled)\n", d);
    }
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1]) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        print_labels();
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        uint32_t drive = 0;
        if (argc < 4 || parse_drive(argv[2], &drive) != 0 || !argv[3] || !argv[3][0]) {
            usage();
            return 1;
        }
        if (eyn_sys_drive_is_present(drive) <= 0) {
            puts("drivelabel: drive not present");
            return 1;
        }
        if (eyn_sys_systemcfg_set_drive_label(drive, argv[3]) != 0) {
            puts("drivelabel: invalid label or failed to persist");
            return 1;
        }
        printf("Drive %u label set to %s\n", drive, argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        uint32_t drive = 0;
        if (argc < 3 || parse_drive(argv[2], &drive) != 0) {
            usage();
            return 1;
        }
        if (eyn_sys_drive_is_present(drive) <= 0) {
            puts("drivelabel: drive not present");
            return 1;
        }
        if (eyn_sys_systemcfg_set_drive_label(drive, "") != 0) {
            puts("drivelabel: failed to clear label");
            return 1;
        }
        printf("Drive %u label cleared\n", drive);
        return 0;
    }

    usage();
    return 1;
}
