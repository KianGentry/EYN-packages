#ifndef EYN_INSTALL_PKGMETA_H
#define EYN_INSTALL_PKGMETA_H

#include <stddef.h>

int pkgmeta_read_file(const char* path,
                      char* out_name,
                      size_t out_name_cap,
                      int* out_version);

#endif