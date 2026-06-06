#include <unistd.h>
#include <errno.h>

char* crypt(const char* key, const char* salt) {
    (void)key;
    (void)salt;
    errno = ENOSYS;
    return 0;
}
