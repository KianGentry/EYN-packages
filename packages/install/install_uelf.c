#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <notify.h>

#include <install/index.h>
#include <install/local.h>
#include <install/package.h>
#include <install/resolve.h>

EYN_CMDMETA_V1("Install, update, remove, and search packages.", "install --update-all");

static void install_usage(void) {
    puts("Usage: install <package>\n"
         "       install --update-all\n"
         "       install --update <package>\n"
         "       install --remove <package>\n"
         "       install --list\n"
         "       install --search <package>\n"
         "       install --check-updates [--notify] [--quiet]\n"
         "Examples:\n"
         "  install hello\n"
         "  install --update-all\n"
         "  install --remove hello\n"
         "  install --search ping");
}

static int install_ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

static int install_str_eq_ci(const char* a, const char* b) {
    if (!a || !b) return 0;
    int i = 0;
    while (a[i] && b[i]) {
        if (install_ascii_lower(a[i]) != install_ascii_lower(b[i])) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int install_parse_version_int(const char* text) {
    if (!text || !text[0]) return -1;

    int value = 0;
    int digits = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9') break;
        int next = value * 10 + (text[i] - '0');
        if (next < value) return -1;
        value = next;
        digits++;
    }
    if (digits == 0) return -1;
    return value;
}

static const Package* install_index_find_ci(const PackageIndex* index, const char* name) {
    if (!index || !name || !name[0]) return NULL;

    for (int i = 0; i < index->count; i++) {
        if (install_str_eq_ci(index->packages[i].name, name)) {
            return &index->packages[i];
        }
    }

    return NULL;
}

static int install_scan_local(LocalPackageSet* out_local) {
    if (!out_local) return -1;
    if (local_packages_scan(out_local) != 0) {
        memset(out_local, 0, sizeof(*out_local));
        return -1;
    }
    return 0;
}

static int install_fetch_index_cached_first(PackageIndex* out_index) {
    if (!out_index) return -1;
    puts("install: loading package index...");
    fflush(stdout);
    return index_fetch_and_parse(out_index);
}

static int install_fetch_index_network(PackageIndex* out_index) {
    if (!out_index) return -1;
    puts("install: querying package server index...");
    fflush(stdout);
    return index_fetch_and_parse_network(out_index);
}

static int install_apply_plan(const PackageIndex* index,
                              const char* requested_name,
                              int only_if_newer,
                              int print_skips,
                              int* out_installed_count) {
    if (!index || !requested_name || !requested_name[0]) return -1;

    ResolvePlan plan;
    if (resolve_install_plan(index, requested_name, &plan) != 0) {
        return -1;
    }

    if (plan.count <= 0) {
        if (out_installed_count) *out_installed_count = 0;
        return 0;
    }

    LocalPackageSet local;
    (void)install_scan_local(&local);

    int installed_count = 0;
    for (int i = 0; i < plan.count; i++) {
        const Package* pkg = plan.ordered[i];
        int remote_version = install_parse_version_int(pkg->version);
        const LocalPackage* local_pkg = local_packages_find(&local, pkg->name);
        int local_version = local_pkg ? local_pkg->version : -1;

        if (only_if_newer
            && local_pkg
            && remote_version >= 0
            && local_version >= remote_version) {
            if (print_skips) {
                printf("install: %s is up to date (%d)\n", pkg->name, local_version);
            }
            continue;
        }

        printf("install: [%d/%d] %s@%s\n", i + 1, plan.count, pkg->name, pkg->version);
        if (install_package(index, pkg) != 0) {
            printf("install: failed while installing %s\n", pkg->name);
            return -1;
        }

        installed_count++;
        (void)install_scan_local(&local);
    }

    if (out_installed_count) *out_installed_count = installed_count;
    return 0;
}

static int install_list_local_packages(void) {
    LocalPackageSet local;
    if (install_scan_local(&local) != 0) {
        puts("install: failed to read /binaries");
        return 1;
    }

    if (local.count == 0) {
        puts("install: no installed packages found");
        return 0;
    }

    printf("install: installed packages (%d)\n", local.count);
    for (int i = 0; i < local.count; i++) {
        if (local.entries[i].has_metadata) {
            printf("  %s @ %d\n", local.entries[i].name, local.entries[i].version);
        } else {
            printf("  %s @ unknown\n", local.entries[i].name);
        }
    }

    return 0;
}

static int install_search_package(const char* query) {
    if (!query || !query[0]) return 1;

    LocalPackageSet local;
    (void)install_scan_local(&local);

    const LocalPackage* local_pkg = local_packages_find_ci(&local, query);
    if (local_pkg) {
        if (local_pkg->has_metadata) {
            printf("install: %s is installed (version %d)\n", local_pkg->name, local_pkg->version);
        } else {
            printf("install: %s is installed\n", local_pkg->name);
        }
        return 0;
    }

    PackageIndex index;
    if (install_fetch_index_cached_first(&index) != 0) {
        puts("install: failed to query package server");
        return 1;
    }

    const Package* server_pkg = install_index_find_ci(&index, query);
    if (!server_pkg) {
        // Cache may be stale; retry a direct network fetch before concluding.
        if (install_fetch_index_network(&index) == 0) {
            server_pkg = install_index_find_ci(&index, query);
        }
    }

    if (server_pkg) {
        printf("install: %s is available on the package server (version %s)\n",
               server_pkg->name,
               server_pkg->version);
        return 0;
    }

    printf("install: package '%s' does not exist on the package server\n", query);
    return 1;
}

static int install_remove_package(const char* name) {
    if (!name || !name[0]) return 1;

    LocalPackageSet local;
    if (install_scan_local(&local) != 0) {
        puts("install: failed to read /binaries");
        return 1;
    }

    const LocalPackage* target = local_packages_find_ci(&local, name);
    if (!target) {
        printf("install: %s is not installed\n", name);
        return 1;
    }

    if (local_packages_remove(target) != 0) {
        printf("install: failed to remove %s (%s)\n", target->name, target->path);
        return 1;
    }

    printf("install: removed %s (%s)\n", target->name, target->path);
    return 0;
}

static int install_check_updates(int notify_user, int quiet) {
    LocalPackageSet local;
    if (install_scan_local(&local) != 0) {
        if (!quiet) puts("install: failed to read installed package list");
        return 1;
    }

    PackageIndex index;
    if (install_fetch_index_cached_first(&index) != 0) {
        if (!quiet) puts("install: failed to fetch package server index");
        return 1;
    }

    int updates = 0;
    for (int i = 0; i < local.count; i++) {
        const Package* server_pkg = index_find_package(&index, local.entries[i].name);
        if (!server_pkg) continue;

        int remote_version = install_parse_version_int(server_pkg->version);
        if (remote_version < 0) continue;

        if (remote_version > local.entries[i].version) {
            updates++;
        }
    }

    if (!quiet) {
        if (updates > 0) {
            printf("install: %d package update(s) available\n", updates);
        } else {
            puts("install: all installed packages are up to date");
        }
    }

    if (notify_user && updates > 0) {
        char message[128];
        int needed = snprintf(message,
                              sizeof(message),
                              "%d package update(s) available. Run install --update-all",
                              updates);
        if (needed > 0 && needed < (int)sizeof(message)) {
            (void)eyn_notify_post("Package updates", message, EYN_NOTIFY_WARNING, 9000);
        }
    }

    return 0;
}

static int install_update_one(const char* name) {
    if (!name || !name[0]) return 1;

    LocalPackageSet local;
    if (install_scan_local(&local) != 0) {
        puts("install: failed to read installed package list");
        return 1;
    }

    if (!local_packages_find_ci(&local, name)) {
        printf("install: %s is not installed\n", name);
        return 1;
    }

    PackageIndex index;
    if (install_fetch_index_cached_first(&index) != 0) {
        puts("install: failed to fetch package server index");
        return 1;
    }

    const Package* pkg = install_index_find_ci(&index, name);
    if (!pkg) {
        printf("install: package '%s' was not found on the package server\n", name);
        return 1;
    }

    int installed_count = 0;
    if (install_apply_plan(&index, pkg->name, 1, 1, &installed_count) != 0) {
        return 1;
    }

    if (installed_count == 0) {
        printf("install: %s is already up to date\n", pkg->name);
    } else {
        printf("install: updated %s (%d package(s) applied)\n", pkg->name, installed_count);
    }

    return 0;
}

static int install_update_all(void) {
    LocalPackageSet local;
    if (install_scan_local(&local) != 0) {
        puts("install: failed to read installed package list");
        return 1;
    }

    if (local.count == 0) {
        puts("install: no installed packages to update");
        return 0;
    }

    PackageIndex index;
    if (install_fetch_index_cached_first(&index) != 0) {
        puts("install: failed to fetch package server index");
        return 1;
    }

    int updated_total = 0;
    int unknown_local_version = 0;
    for (int i = 0; i < local.count; i++) {
        if (!local.entries[i].has_metadata) {
            unknown_local_version++;
        }

        const Package* pkg = install_index_find_ci(&index, local.entries[i].name);
        if (!pkg) continue;

        int installed_count = 0;
        if (install_apply_plan(&index, pkg->name, 1, 0, &installed_count) != 0) {
            return 1;
        }
        updated_total += installed_count;
    }

    if (updated_total == 0) {
        if (unknown_local_version > 0) {
            printf("install: no updates were needed (%d package(s) had unknown local version)\n",
                   unknown_local_version);
        } else {
            puts("install: no updates were needed");
        }
    } else {
        if (unknown_local_version > 0) {
            printf("install: update complete (%d package(s) updated; %d with unknown local version were checked)\n",
                   updated_total,
                   unknown_local_version);
        } else {
            printf("install: update complete (%d package(s) updated)\n", updated_total);
        }
    }

    return 0;
}

static int install_install_one(const char* requested_name) {
    if (!requested_name || !requested_name[0]) return 1;

    PackageIndex index;
    if (install_fetch_index_network(&index) != 0) {
        if (install_fetch_index_cached_first(&index) != 0) {
            return 1;
        }
    }

    const Package* pkg = install_index_find_ci(&index, requested_name);
    if (!pkg) {
        printf("install: package not found in index: %s\n", requested_name);
        return 1;
    }

    int installed_count = 0;
    if (install_apply_plan(&index, pkg->name, 1, 1, &installed_count) != 0) {
        return 1;
    }

    if (installed_count == 0) {
        printf("install: %s is already up to date\n", pkg->name);
    } else {
        printf("install: complete (%d package(s))\n", installed_count);
    }

    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        install_usage();
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        install_usage();
        return 0;
    }

    if (strcmp(argv[1], "--list") == 0 || strcmp(argv[1], "-l") == 0) {
        if (argc != 2) {
            install_usage();
            return 1;
        }
        return install_list_local_packages();
    }

    if (strcmp(argv[1], "--search") == 0 || strcmp(argv[1], "-s") == 0) {
        if (argc != 3) {
            install_usage();
            return 1;
        }
        return install_search_package(argv[2]);
    }

    if (strcmp(argv[1], "--remove") == 0 || strcmp(argv[1], "-r") == 0) {
        if (argc != 3) {
            install_usage();
            return 1;
        }
        return install_remove_package(argv[2]);
    }

    if (strcmp(argv[1], "--update") == 0 || strcmp(argv[1], "-u") == 0) {
        if (argc != 3) {
            install_usage();
            return 1;
        }
        return install_update_one(argv[2]);
    }

    if (strcmp(argv[1], "--update-all") == 0 || strcmp(argv[1], "-U") == 0) {
        if (argc != 2) {
            install_usage();
            return 1;
        }
        return install_update_all();
    }

    if (strcmp(argv[1], "--check-updates") == 0) {
        int notify_user = 0;
        int quiet = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--notify") == 0) {
                notify_user = 1;
            } else if (strcmp(argv[i], "--quiet") == 0) {
                quiet = 1;
            } else {
                install_usage();
                return 1;
            }
        }
        return install_check_updates(notify_user, quiet);
    }

    if (argv[1][0] == '-') {
        install_usage();
        return 1;
    }

    if (argc != 2) {
        install_usage();
        return 1;
    }

    return install_install_one(argv[1]);
}
