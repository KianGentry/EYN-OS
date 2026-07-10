#pragma once

typedef enum {
    EYN_NOTIFY_INFO = 0,
    EYN_NOTIFY_WARNING = 1,
    EYN_NOTIFY_ERROR = 2,
} eyn_notify_level_t;

int eyn_notify_post(const char* title,
                    const char* message,
                    eyn_notify_level_t level,
                    unsigned timeout_ms);

int eyn_notify_post_wait(const char* title,
                         const char* message,
                         eyn_notify_level_t level,
                         unsigned timeout_ms);
