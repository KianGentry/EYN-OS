#include <libgen.h>
#include <string.h>

char* basename(char* path) {
    if (!path || !*path) return (char*)".";

    // Strip trailing slashes
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }

    char* last = strrchr(path, '/');
    return last ? (last + 1) : path;
}

char* dirname(char* path) {
    if (!path || !*path) return (char*)".";

    // Strip trailing slashes
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }

    char* last = strrchr(path, '/');
    if (!last) return (char*)".";
    if (last == path) {
        // root
        path[1] = '\0';
        return path;
    }
    *last = '\0';
    return path;
}
