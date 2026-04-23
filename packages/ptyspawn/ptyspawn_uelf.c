#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Spawn a child process on a PTY slave and relay its output.",
               "ptyspawn /binaries/help");

static void usage(void) {
    puts("Usage: ptyspawn <path> [args...]\n"
         "Example: ptyspawn /binaries/echo hello\n"
         "Note: path should be an executable path visible to spawn().");
}

int main(int argc, char** argv) {
    if (argc < 2 || (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0)) {
        usage();
        return (argc < 2) ? 1 : 0;
    }

    int pty_fds[2] = {-1, -1};
    if (eyn_sys_pty_open(pty_fds) != 0) {
        puts("ptyspawn: failed to allocate PTY pair");
        return 1;
    }

    int master_fd = pty_fds[0];
    int slave_fd = pty_fds[1];

    if (fd_set_nonblock(master_fd, 1) != 0) {
        puts("ptyspawn: warning: could not set master PTY nonblocking");
    }

    eyn_spawn_ex_req_t req;
    req.path = argv[1];
    req.argv = (const char* const*)&argv[1];
    req.argc = argc - 1;
    req.stdin_fd = slave_fd;
    req.stdout_fd = slave_fd;
    req.stderr_fd = slave_fd;
    req.inherit_mode = 1;

    int pid = eyn_sys_spawn_ex(&req);
    if (pid <= 0) {
        puts("ptyspawn: spawn_ex failed");
        (void)close(master_fd);
        (void)close(slave_fd);
        return 1;
    }

    (void)close(slave_fd);

    int status = 0;
    int exited = 0;
    int empty_after_exit = 0;

    for (;;) {
        char buf[256];
        ssize_t n = read(master_fd, buf, sizeof(buf));
        if (n > 0) {
            (void)write(1, buf, (size_t)n);
            empty_after_exit = 0;
        }

        if (!exited) {
            int wr = waitpid(pid, &status, WNOHANG);
            if (wr == pid) {
                exited = 1;
            }
        }

        if (exited) {
            if (n <= 0) {
                empty_after_exit++;
                if (empty_after_exit >= 4) break;
            }
        }

        (void)usleep(10000);
    }

    (void)close(master_fd);

    if (status != 0) {
        printf("\nptyspawn: child exited with status %d\n", status);
    }

    return status;
}
