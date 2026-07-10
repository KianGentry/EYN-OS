#pragma once

/*
 * Optional command metadata consumed by the kernel's `help` command.
 *
 * Format (section: `.eynos.cmdmeta`):
 *   - magic:  "ECMD" (4 bytes)
 *   - version: u16 little-endian (currently 1)
 *   - reserved: u16 (0)
 *   - description: NUL-terminated string
 *   - example:     NUL-terminated string
 */

#define EYN_CMDMETA__CAT(a, b) a##b
#define EYN_CMDMETA__CAT2(a, b) EYN_CMDMETA__CAT(a, b)

/*
 * Emit v1 metadata.
 *
 * Usage:
 *   EYN_CMDMETA_V1("List directory entries.", "files /");
 */
#define EYN_CMDMETA_V1(desc, example) \
    __attribute__((section(".eynos.cmdmeta"), used)) \
    static const unsigned char EYN_CMDMETA__CAT2(g_cmdmeta_, __COUNTER__)[] = \
        "ECMD" "\x01\x00\x00\x00" desc "\0" example "\0"
