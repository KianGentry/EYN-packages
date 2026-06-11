#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("List logical drives and partitions.", "drives");

static void usage(void) {
    puts("Usage: drives");
}

int main(int argc, char** argv) {
    int count;
    int current;
    int install;
    int i;

    if (argc > 1 && argv[1] && argv[1][0]) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage();
            return 0;
        }
        usage();
        return 1;
    }

    count = eyn_sys_drive_get_count();
    if (count <= 0) {
        puts("drives: no drives found");
        return 1;
    }

    current = eyn_sys_drive_get_logical();
    install = eyn_sys_systemcfg_get_install_drive();

    printf("Logical drives: %d\n", count);
    for (i = 0; i < count; ++i) {
        char label[32];
        int is_present = eyn_sys_drive_is_present((uint32_t)i);
        int is_ram = (i == (count - 1));
        int label_n;

        if (is_present <= 0) continue;

        label[0] = '\0';
        label_n = eyn_sys_systemcfg_get_drive_label((uint32_t)i, label, (int)sizeof(label));

        printf("- %d: %s", i, is_ram ? "RAM" : "disk");
        if (label_n > 0 && label[0]) printf(" label=%s", label);
        if (i == current) printf(" [current]");
        if (!is_ram && i == install) printf(" [install]");
        printf("\n");

        if (!is_ram) {
            eyn_installer_partitions_t parts;
            memset(&parts, 0, sizeof(parts));
            if (eyn_sys_installer_get_partitions((uint32_t)i, &parts) == 0) {
                uint32_t pcount = parts.partition_count;
                uint32_t p;
                if (pcount > 4u) pcount = 4u;
                if (pcount == 0u) {
                    puts("    partitions: none");
                } else {
                    for (p = 0; p < pcount; ++p) {
                        const eyn_installer_partition_t* part = &parts.partitions[p];
                        printf("    p%u: type=0x%02x boot=%u lba=%u sectors=%u\n",
                               p,
                               (unsigned)part->type,
                               (unsigned)part->bootable,
                               (unsigned)part->lba_start,
                               (unsigned)part->sector_count);
                    }
                }
            }
        }
    }

    return 0;
}
