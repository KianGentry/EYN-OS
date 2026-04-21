#include <notify.h>

#include <eynos_syscall.h>

int eyn_notify_post(const char* title,
                    const char* message,
                    eyn_notify_level_t level,
                    unsigned timeout_ms) {
    const char* resolved_title = (title && title[0]) ? title : "Notification";
    const char* resolved_message = (message && message[0]) ? message : "(empty)";

    int clamped_level = (int)level;
    if (clamped_level < 0) clamped_level = EYN_NOTIFY_INFO;
    if (clamped_level > EYN_NOTIFY_ERROR) clamped_level = EYN_NOTIFY_ERROR;

    unsigned effective_timeout = timeout_ms;
    if (effective_timeout == 0) effective_timeout = 7000u;

    return eyn_sys_notify_post(resolved_title,
                               resolved_message,
                               clamped_level,
                               (uint32_t)effective_timeout);
}

int eyn_notify_post_wait(const char* title,
                         const char* message,
                         eyn_notify_level_t level,
                         unsigned timeout_ms) {
    return eyn_notify_post(title, message, level, timeout_ms);
}
