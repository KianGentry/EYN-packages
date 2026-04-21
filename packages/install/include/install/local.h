#ifndef EYN_INSTALL_LOCAL_H
#define EYN_INSTALL_LOCAL_H

#include "package.h"

#define LOCAL_MAX_PACKAGES 256
#define LOCAL_PATH_MAX 256

typedef struct LocalPackage {
    char name[MAX_NAME];
    int version;
    int has_metadata;
    char path[LOCAL_PATH_MAX];
} LocalPackage;

typedef struct LocalPackageSet {
    LocalPackage entries[LOCAL_MAX_PACKAGES];
    int count;
} LocalPackageSet;

int local_packages_scan(LocalPackageSet* out_set);
const LocalPackage* local_packages_find(const LocalPackageSet* set, const char* name);
const LocalPackage* local_packages_find_ci(const LocalPackageSet* set, const char* name);
int local_packages_remove(const LocalPackage* pkg);

#endif