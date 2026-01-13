#pragma once

// Minimal glob() stubs. EYN-OS userland does not provide a filesystem glob.

typedef struct {
    unsigned int gl_pathc;
    char** gl_pathv;
} glob_t;

static inline int glob(const char* pattern, int flags, void* errfunc, glob_t* pglob) {
    (void)pattern; (void)flags; (void)errfunc;
    if (pglob) { pglob->gl_pathc = 0; pglob->gl_pathv = 0; }
    return 0;
}

static inline void globfree(glob_t* pglob) {
    (void)pglob;
}
