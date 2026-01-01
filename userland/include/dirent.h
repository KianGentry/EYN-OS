#include <stddef.h>
#include <stdint.h>

// Fixed-size directory entry record returned by getdents().
// Buffer passed to getdents() should be an array of these.
typedef struct {
    uint8_t is_dir;
    uint8_t _pad[3];
    uint32_t size;
    char name[56];
} eyn_dirent_t;

int getdents(int fd, eyn_dirent_t* out, size_t bytes);
