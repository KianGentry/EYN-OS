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
 *  - Higher-level UI should prefer hal_kbd_read_key_nonblock() which returns a
 *    unified keycode (ASCII for printable keys, and stable non-ASCII codes for
 *    arrows/home/end/delete/etc).
 *  - hal_kbd_getc_nonblock() remains as a legacy byte stream for early bring-up
 *    and simple line editors; it returns ASCII only and will drop non-ASCII
 *    special keys.
 */

/*
 * Unified keycodes.
 *
 * Encoding notes:
 *  - ASCII keys are returned as their byte value (1..255).
 *  - Special keys use 0x1000+ codes. This matches the existing i386 TUI key
 *    encoding so shared UI code can be migrated with minimal churn.
 *  - Some modifiers are encoded into the returned key using OR flags to match
 *    existing UI behavior (notably Shift-selection and Super+Arrows).
 */
#define HAL_KEY_UP      0x1001u
#define HAL_KEY_DOWN    0x1002u
#define HAL_KEY_LEFT    0x1003u
#define HAL_KEY_RIGHT   0x1004u
#define HAL_KEY_DELETE  0x1005u
#define HAL_KEY_HOME    0x1006u
#define HAL_KEY_END     0x1007u
#define HAL_KEY_PGUP    0x1008u
#define HAL_KEY_PGDN    0x1009u

/* Keycode flags (OR'd into the returned keycode). */
#define HAL_KEY_FLAG_SHIFTSEL 0x3000u /* used for Shift+Arrow selection extension */
#define HAL_KEY_FLAG_SUPER    0x4000u /* Super/Meta key held during press */

/* Modifier flags (optional; best-effort for backends that can report them). */
#define HAL_KBD_MOD_SHIFT  (1u << 0)
#define HAL_KBD_MOD_CTRL   (1u << 1)
#define HAL_KBD_MOD_ALT    (1u << 2)
#define HAL_KBD_MOD_SUPER  (1u << 3)
#define HAL_KBD_MOD_CAPS   (1u << 4)

/* Returns 0 if no character is available; otherwise returns the character byte. */
int hal_kbd_getc_nonblock(void);

/* Returns 0 if no key is available; otherwise returns a unified keycode. */
uint32 hal_kbd_read_key_nonblock(void);

/* Returns a bitmask of HAL_KBD_MOD_* flags. Safe to return 0 if unsupported. */
uint32 hal_kbd_modifiers(void);

#endif
