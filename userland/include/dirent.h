#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Fixed-size directory entry record returned by getdents().
// Buffer passed to getdents() should be an array of these.
typedef struct {
    uint8_t is_dir;
    uint8_t _pad[3];
    uint32_t size;
    char name[56];
} eyn_dirent_t;

typedef struct {
    int fd;
    int eof;
} DIR;

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

int getdents(int fd, eyn_dirent_t* out, size_t bytes);
DIR* opendir(const char* path);
DIR* fdopendir(int fd);
struct dirent* readdir(DIR* dirp);
int closedir(DIR* dirp);
int dirfd(DIR* dirp);
