#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Drive selection helper.", "drive 0");

static int is_ram_token(const char* s) {
    if (!s) return 0;
    return ((s[0] == 'R' || s[0] == 'r') &&
            (s[1] == 'A' || s[1] == 'a') &&
            (s[2] == 'M' || s[2] == 'm') &&
            s[3] == '\0');
}

static int resolve_target(const char* token, uint32_t* out_logical) {
    char* end = NULL;
    unsigned long logical;
    int count;
    int i;

    if (!token || !out_logical) return -1;

    logical = strtoul(token, &end, 10);
    if (end && *end == '\0') {
        *out_logical = (uint32_t)logical;
        return 0;
    }

    count = eyn_sys_drive_get_count();
    if (count <= 0) return -1;

    if (is_ram_token(token)) {
        *out_logical = (uint32_t)(count - 1);
        return 0;
    }

    for (i = 0; i < count; ++i) {
        char label[32];
        int n;
        if (eyn_sys_drive_is_present((uint32_t)i) <= 0) continue;
        label[0] = '\0';
        n = eyn_sys_systemcfg_get_drive_label((uint32_t)i, label, (int)sizeof(label));
        if (n > 0 && strcmp(label, token) == 0) {
            *out_logical = (uint32_t)i;
            return 0;
        }
    }

    return -1;
}

static void usage(void) {
    puts("Usage: drive <logical-drive|label|RAM>");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    uint32_t logical = 0;
    if (resolve_target(argv[1], &logical) != 0) {
        puts("drive: unknown drive target");
        return 1;
    }

    if (eyn_sys_drive_is_present(logical) <= 0) {
        puts("drive: target drive not present");
        return 1;
    }

    int rc = eyn_sys_drive_set_logical(logical);
    if (rc < 0) {
        puts("drive: failed to switch drive");
        return 1;
    }

    int cur = eyn_sys_drive_get_logical();
    if (cur < 0) cur = rc;
    {
        int total = eyn_sys_drive_get_count();
        if (total > 0 && cur == (total - 1)) {
            puts("Switched to drive RAM");
        } else {
            char label[32];
            label[0] = '\0';
            if (eyn_sys_systemcfg_get_drive_label((uint32_t)cur, label, (int)sizeof(label)) > 0 && label[0]) {
                printf("Switched to drive %d (%s)\n", cur, label);
            } else {
                printf("Switched to drive %d\n", cur);
            }
        }
    }
    return 0;
}
