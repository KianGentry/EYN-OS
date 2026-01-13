#include <errno.h>

int errno = 0;

const char* strerror(int errnum) {
    switch (errnum) {
        case 0: return "ok";
        case ENOENT: return "no such file";
        case EINVAL: return "invalid argument";
        case ENOSYS: return "not implemented";
        default: return "error";
    }
}
