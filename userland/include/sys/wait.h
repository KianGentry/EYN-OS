#pragma once

// Minimal wait() stub.

static inline int wait(int* status) {
    (void)status;
    return -1;
}
