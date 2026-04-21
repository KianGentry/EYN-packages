#include "pkgmeta.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PKGMETA_MARKER_LEN 8
#define PKGMETA_SCAN_CHUNK 512
#define PKGMETA_SCAN_OVERLAP (PKGMETA_MARKER_LEN - 1)

static const unsigned char PKGMETA_MARKER[PKGMETA_MARKER_LEN] = {
    'E', 'P', 'K', 'G', 0x01, 0x00, 0x00, 0x00
};

static int pkgmeta_marker_match(const unsigned char* p) {
    if (!p) return 0;
    for (int i = 0; i < PKGMETA_MARKER_LEN; i++) {
        if (p[i] != PKGMETA_MARKER[i]) return 0;
    }
    return 1;
}

static int pkgmeta_parse_version(const char* text, int* out_version) {
    if (!text || !out_version) return -1;

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
    *out_version = value;
    return 0;
}

static int pkgmeta_extract_after_marker(int fd,
                                        const unsigned char* initial,
                                        int initial_len,
                                        char* out_name,
                                        size_t out_name_cap,
                                        int* out_version) {
    if (!out_name || out_name_cap == 0 || !out_version) return -1;

    unsigned char strbuf[320];
    int have = initial_len;
    if (have < 0) have = 0;
    if (have > (int)sizeof(strbuf)) have = (int)sizeof(strbuf);
    if (have > 0 && initial) {
        memcpy(strbuf, initial, (size_t)have);
    }

    while (have < (int)sizeof(strbuf) - 1) {
        int nul_count = 0;
        for (int i = 0; i < have; i++) {
            if (strbuf[i] == 0 && ++nul_count >= 2) break;
        }
        if (nul_count >= 2) break;

        int rd = read(fd, strbuf + have, (size_t)((int)sizeof(strbuf) - have - 1));
        if (rd <= 0) break;
        have += rd;
    }

    int nul1 = -1;
    int nul2 = -1;
    for (int i = 0; i < have; i++) {
        if (strbuf[i] == 0) {
            nul1 = i;
            break;
        }
    }
    if (nul1 < 0) return -1;

    for (int i = nul1 + 1; i < have; i++) {
        if (strbuf[i] == 0) {
            nul2 = i;
            break;
        }
    }
    if (nul2 < 0) return -1;

    int name_len = nul1;
    if (name_len <= 0) return -1;
    if ((size_t)name_len >= out_name_cap) {
        name_len = (int)out_name_cap - 1;
    }
    memcpy(out_name, strbuf, (size_t)name_len);
    out_name[name_len] = '\0';

    const char* version_text = (const char*)(strbuf + nul1 + 1);
    if (pkgmeta_parse_version(version_text, out_version) != 0) {
        return -1;
    }

    return 0;
}

static int pkgmeta_scan_fd(int fd,
                           char* out_name,
                           size_t out_name_cap,
                           int* out_version) {
    unsigned char buf[PKGMETA_SCAN_CHUNK + PKGMETA_SCAN_OVERLAP];
    int used = 0;

    for (;;) {
        int rd = read(fd, buf + used, PKGMETA_SCAN_CHUNK);
        if (rd <= 0) break;
        used += rd;

        for (int i = 0; i + PKGMETA_MARKER_LEN <= used; i++) {
            if (!pkgmeta_marker_match(buf + i)) continue;

            int tail_len = used - i - PKGMETA_MARKER_LEN;
            const unsigned char* tail = buf + i + PKGMETA_MARKER_LEN;
            return pkgmeta_extract_after_marker(fd,
                                                tail,
                                                tail_len,
                                                out_name,
                                                out_name_cap,
                                                out_version);
        }

        if (used > PKGMETA_SCAN_OVERLAP) {
            memmove(buf, buf + used - PKGMETA_SCAN_OVERLAP, PKGMETA_SCAN_OVERLAP);
            used = PKGMETA_SCAN_OVERLAP;
        }
    }

    return -1;
}

int pkgmeta_read_file(const char* path,
                      char* out_name,
                      size_t out_name_cap,
                      int* out_version) {
    if (!path || !out_name || out_name_cap == 0 || !out_version) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    int rc = pkgmeta_scan_fd(fd, out_name, out_name_cap, out_version);
    close(fd);
    return rc;
}