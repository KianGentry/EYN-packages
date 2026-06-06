#include <dirent.h>
#include <eynos_syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int getdents(int fd, eyn_dirent_t* out, size_t bytes) {
    if (!out) return -1;
    if (bytes > 0x7fffffffU) bytes = 0x7fffffffU;
    return eyn_syscall3(EYN_SYSCALL_GETDENTS, fd, out, (int)bytes);
}

DIR* opendir(const char* path) {
    if (!path) return NULL;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    return fdopendir(fd);
}

DIR* fdopendir(int fd) {
    if (fd < 0) return NULL;
    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (!d) {
        (void)close(fd);
        return NULL;
    }
    d->fd = fd;
    d->eof = 0;
    return d;
}

struct dirent* readdir(DIR* dirp) {
    static struct dirent out;
    eyn_dirent_t raw;

    if (!dirp || dirp->eof) return NULL;
    int rc = getdents(dirp->fd, &raw, sizeof(raw));
    if (rc <= 0) {
        dirp->eof = 1;
        return NULL;
    }

    out.d_ino = 0;
    out.d_type = raw.is_dir ? DT_DIR : DT_REG;
    (void)strncpy(out.d_name, raw.name, sizeof(out.d_name));
    out.d_name[sizeof(out.d_name) - 1] = '\0';
    return &out;
}

int closedir(DIR* dirp) {
    if (!dirp) return -1;
    int fd = dirp->fd;
    free(dirp);
    return close(fd);
}

int dirfd(DIR* dirp) {
    if (!dirp) return -1;
    return dirp->fd;
}
