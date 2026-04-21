#include "local.h"

#include "pkgmeta.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOCAL_BIN_DIR "/binaries"

static int local_ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

static int local_name_cmp_ci(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    int i = 0;
    while (a[i] && b[i]) {
        int ca = local_ascii_lower(a[i]);
        int cb = local_ascii_lower(b[i]);
        if (ca != cb) return ca - cb;
        i++;
    }

    return local_ascii_lower(a[i]) - local_ascii_lower(b[i]);
}

static int local_add_or_replace(LocalPackageSet* set,
                                const char* name,
                                int version,
                                int has_metadata,
                                const char* path) {
    if (!set || !name || !name[0] || !path || !path[0]) return -1;

    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->entries[i].name, name) != 0) continue;

        int replace = 0;
        if (has_metadata && !set->entries[i].has_metadata) {
            replace = 1;
        } else if (has_metadata == set->entries[i].has_metadata
                   && version > set->entries[i].version) {
            replace = 1;
        }

        if (replace) {
            strncpy(set->entries[i].name, name, sizeof(set->entries[i].name) - 1);
            set->entries[i].name[sizeof(set->entries[i].name) - 1] = '\0';
            set->entries[i].version = version;
            set->entries[i].has_metadata = has_metadata;
            strncpy(set->entries[i].path, path, sizeof(set->entries[i].path) - 1);
            set->entries[i].path[sizeof(set->entries[i].path) - 1] = '\0';
        }
        return 0;
    }

    if (set->count >= LOCAL_MAX_PACKAGES) return -1;

    LocalPackage* dst = &set->entries[set->count++];
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->name, name, sizeof(dst->name) - 1);
    dst->name[sizeof(dst->name) - 1] = '\0';
    dst->version = version;
    dst->has_metadata = has_metadata;
    strncpy(dst->path, path, sizeof(dst->path) - 1);
    dst->path[sizeof(dst->path) - 1] = '\0';
    return 0;
}

static void local_sort(LocalPackageSet* set) {
    if (!set) return;

    for (int i = 0; i < set->count - 1; i++) {
        for (int j = i + 1; j < set->count; j++) {
            if (local_name_cmp_ci(set->entries[i].name, set->entries[j].name) > 0) {
                LocalPackage tmp = set->entries[i];
                set->entries[i] = set->entries[j];
                set->entries[j] = tmp;
            }
        }
    }
}

int local_packages_scan(LocalPackageSet* out_set) {
    if (!out_set) return -1;
    memset(out_set, 0, sizeof(*out_set));

    int fd = open(LOCAL_BIN_DIR, O_RDONLY, 0);
    if (fd < 0) return -1;

    eyn_dirent_t entries[16];
    for (;;) {
        int rc = getdents(fd, entries, sizeof(entries));
        if (rc <= 0) break;

        int count = rc / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; i++) {
            if (entries[i].is_dir) continue;
            if (!entries[i].name[0]) continue;

            char path[LOCAL_PATH_MAX];
            int needed = snprintf(path, sizeof(path), "%s/%s", LOCAL_BIN_DIR, entries[i].name);
            if (needed <= 0 || needed >= (int)sizeof(path)) continue;

            char package_name[MAX_NAME];
            int package_version = 0;
            int has_metadata = 0;

            if (pkgmeta_read_file(path,
                                  package_name,
                                  sizeof(package_name),
                                  &package_version) == 0) {
                has_metadata = 1;
            } else {
                strncpy(package_name, entries[i].name, sizeof(package_name) - 1);
                package_name[sizeof(package_name) - 1] = '\0';
                package_version = 0;
            }

            (void)local_add_or_replace(out_set,
                                       package_name,
                                       package_version,
                                       has_metadata,
                                       path);
        }
    }

    close(fd);
    local_sort(out_set);
    return 0;
}

const LocalPackage* local_packages_find(const LocalPackageSet* set, const char* name) {
    if (!set || !name || !name[0]) return NULL;

    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->entries[i].name, name) == 0) {
            return &set->entries[i];
        }
    }

    return NULL;
}

const LocalPackage* local_packages_find_ci(const LocalPackageSet* set, const char* name) {
    if (!set || !name || !name[0]) return NULL;

    for (int i = 0; i < set->count; i++) {
        if (local_name_cmp_ci(set->entries[i].name, name) == 0) {
            return &set->entries[i];
        }
    }

    return NULL;
}

int local_packages_remove(const LocalPackage* pkg) {
    if (!pkg || !pkg->path[0]) return -1;
    return unlink(pkg->path);
}