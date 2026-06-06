#pragma once

#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WIFEXITED(status)   (WTERMSIG(status) == 0)

int wait(int* status);
int waitpid(int pid, int* status, int options);
