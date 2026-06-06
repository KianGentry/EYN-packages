#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

/*
 * fstat -- query metadata for an open file descriptor.
 *
 * EYN-OS VFS does not expose extended metadata (ownership, timestamps) to
 * ring-3 programs.  We fill in st_size by seeking to the end and back, and
 * report st_mode as S_IFREG.  This is sufficient for DOOM's M_ReadFile(),
 * which only needs st_size to determine allocation length.
 */
int fstat(int fd, struct stat* st) {
    if (!st) { errno = EINVAL; return -1; }

    /* Determine current position, seek to end for size, seek back. */
    long cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) { errno = EBADF; return -1; }

    long end = lseek(fd, 0, SEEK_END);
    if (end < 0) { errno = EBADF; return -1; }

    lseek(fd, cur, SEEK_SET);   /* restore position */

    st->st_dev = 0;
    st->st_ino = 0;
    st->st_nlink = 1;
    st->st_mode = S_IFREG;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = end;
    st->st_blksize = 512;
    st->st_blocks = (end + 511) / 512;
    st->st_atime = 0;
    st->st_mtime = 0;
    st->st_ctime = 0;
    return 0;
}

int stat(const char* path, struct stat* st) {
    if (!path || !st) { errno = EINVAL; return -1; }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { errno = ENOENT; return -1; }

    // Determine if directory by attempting getdents.
    eyn_dirent_t dent;
    int grc = getdents(fd, &dent, sizeof(dent));
    if (grc >= 0) {
        st->st_dev = 0;
        st->st_ino = 0;
        st->st_nlink = 1;
        st->st_mode = S_IFDIR;
        st->st_uid = 0;
        st->st_gid = 0;
        st->st_rdev = 0;
        st->st_size = 0;
        st->st_blksize = 512;
        st->st_blocks = 0;
        st->st_atime = 0;
        st->st_mtime = 0;
        st->st_ctime = 0;
        close(fd);
        return 0;
    }

    // File: estimate size by reading to EOF.
    long total = 0;
    for (;;) {
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) { close(fd); errno = EINVAL; return -1; }
        if (n == 0) break;
        total += n;
    }

    close(fd);
    st->st_dev = 0;
    st->st_ino = 0;
    st->st_nlink = 1;
    st->st_mode = S_IFREG;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = total;
    st->st_blksize = 512;
    st->st_blocks = (total + 511) / 512;
    st->st_atime = 0;
    st->st_mtime = 0;
    st->st_ctime = 0;
    return 0;
}

int lstat(const char* path, struct stat* st) {
    return stat(path, st);
}

int mknod(const char* path, mode_t mode, dev_t dev) {
    (void)path;
    (void)mode;
    (void)dev;
    errno = ENOSYS;
    return -1;
}

int fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int chmod(const char* path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) {
    (void)dirfd;
    (void)path;
    (void)times;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
    (void)dirfd;
    (void)path;
    (void)mode;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd;
    (void)owner;
    (void)group;
    errno = ENOSYS;
    return -1;
}

mode_t umask(mode_t mask) {
    (void)mask;
    return 0;
}
