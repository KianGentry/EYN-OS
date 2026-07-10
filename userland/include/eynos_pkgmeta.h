#pragma once

/*
 * Optional package metadata consumed by package-management tooling.
 *
 * Format (section: `.eynos.pkgmeta`):
 *   - magic:   "EPKG" (4 bytes)
 *   - version: u16 little-endian (currently 1)
 *   - reserved: u16 (0)
 *   - package name: NUL-terminated string
 *   - package version: NUL-terminated decimal integer string
 */

#define EYN_PKGMETA__CAT(a, b) a##b
#define EYN_PKGMETA__CAT2(a, b) EYN_PKGMETA__CAT(a, b)
#define EYN_PKGMETA__STR2(v) #v
#define EYN_PKGMETA__STR(v) EYN_PKGMETA__STR2(v)

#define EYN_PKGMETA_V1(name_literal, version_int_literal) \
    __attribute__((section(".eynos.pkgmeta"), used)) \
    static const unsigned char EYN_PKGMETA__CAT2(g_pkgmeta_, __COUNTER__)[] = \
        "EPKG" "\x01\x00\x00\x00" name_literal "\0" EYN_PKGMETA__STR(version_int_literal) "\0"
