#ifndef EYNOS_HAL_KEYBOARD_H
#define EYNOS_HAL_KEYBOARD_H

#include <misc/types.h>

/*
 * HAL Keyboard
 *
 * Purpose:
 *  Provide a single, portable, non-blocking character stream that higher-level
 *  code can consume (shell/TUI/userland input).
 *
 * Contract:
 *  - Returns 0 if no input is available.
 *  - Otherwise returns a byte value (1..255) in the unified input stream.
 *  - Backends are responsible for translating their native key events into the
 *    same stream semantics.
 */

/* Modifier flags (optional; best-effort for backends that can report them). */
#define HAL_KBD_MOD_SHIFT  (1u << 0)
#define HAL_KBD_MOD_CTRL   (1u << 1)
#define HAL_KBD_MOD_ALT    (1u << 2)
#define HAL_KBD_MOD_SUPER  (1u << 3)
#define HAL_KBD_MOD_CAPS   (1u << 4)

/* Returns 0 if no character is available; otherwise returns the character byte. */
int hal_kbd_getc_nonblock(void);

/* Returns a bitmask of HAL_KBD_MOD_* flags. Safe to return 0 if unsupported. */
uint32 hal_kbd_modifiers(void);

#endif
