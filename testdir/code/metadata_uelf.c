#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#include <dirent.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Show metadata for a file or directory.", "metadata /binaries/ping");

// ELF32 magic bytes (first 4 bytes of an ELF file).
static const unsigned char ELF_MAGIC[4] = { 0x7F, 'E', 'L', 'F' };

/* 
    ECMD metadata marker emitted by EYN_CMDMETA_V1.
    ABI-INVARIANT: Must match userland/include/eynos_cmdmeta.h exactly.
    Layout: "ECMD" (4 bytes) | version u16 LE = 1 | reserved u16 = 0 |
    description NUL-string | example NUL-string.
    
    The array is stored XOR'd with 0xFF so that the raw marker sequence is
    never present in this binary's .rodata , without this the scanner would
    hit the constant itself as a false-positive match before reaching the
    actual EYN_CMDMETA_V1 payload.
*/
static const unsigned char ECMD_MARKER_XOR[8] = {
    'E'^0xFF, 'C'^0xFF, 'M'^0xFF, 'D'^0xFF,
    0x01^0xFF, 0x00^0xFF, 0x00^0xFF, 0x00^0xFF
};
#define ECMD_MARKER_LEN 8

static int ecmd_marker_match(const unsigned char* p) {
    for (int i = 0; i < ECMD_MARKER_LEN; i++)
        if (p[i] != (unsigned char)(ECMD_MARKER_XOR[i] ^ 0xFF)) return 0;
    return 1;
}

static const char* basename_of(const char* path) {
    const char* p = path;
    const char* last = path;
    while (*p) {
        if (*p == '/') last = p + 1;
        p++;
    }
    return last;
}

/*
    Try to get the file size by seeking to the end.
    Returns size >= 0 on success, -1 on failure (e.g. directory fd).
*/
static int file_size_from_fd(int fd) {
    eyn_cap_t cap;
    if (eyn_sys_cap_mint_fd(fd, EYN_CAP_R_SEEK, &cap) != 0)
        return -1;
    /* SEEK_END = 2 */
    return eyn_sys_cap_fd_seek(&cap, 0, 2);
}

/*
    Return 1 if fd is a directory (getdents succeeds), 0 otherwise.
    Leaves the fd position unchanged (reads nothing on success because
    getdents on a directory is position-independent).
*/
static int fd_is_dir(int fd) {
    eyn_dirent_t probe;
    return getdents(fd, &probe, sizeof(probe)) >= 0;
}

/*
    Scan fd from current position for the ECMD marker and extract the
    two NUL-terminated metadata strings that follow it.
    Returns 1 on success, 0 if not found.
*/
static int scan_ecmd(int fd, char* desc, int desc_cap,
                     char* example, int example_cap)
{
    enum { CHUNK = 512 };
    // Overlap prevents a marker from being missed when split across reads.
    enum { OVERLAP = ECMD_MARKER_LEN - 1 };

    unsigned char buf[CHUNK + OVERLAP];
    int used = 0;

    for (;;) {
        int n = read(fd, buf + used, CHUNK);
        if (n <= 0) break;
        used += n;

        for (int i = 0; i + ECMD_MARKER_LEN <= used; i++) {
            if (!ecmd_marker_match(buf + i)) continue;

            // Found marker -- buffer the strings that follow.
            unsigned char sb[256];
            int have = used - i - ECMD_MARKER_LEN;
            if (have < 0) have = 0;
            if (have > (int)sizeof(sb)) have = (int)sizeof(sb);
            memcpy(sb, buf + i + ECMD_MARKER_LEN, (unsigned int)have);

            while (have < (int)sizeof(sb) - 1) {
                int nuls = 0;
                for (int k = 0; k < have; k++)
                    if (sb[k] == 0 && ++nuls >= 2) break;
                if (nuls >= 2) break;
                int nn = read(fd, sb + have,
                              (unsigned int)((int)sizeof(sb) - have - 1));
                if (nn <= 0) break;
                have += nn;
            }

            // First string: description.
            int nul1 = -1;
            for (int k = 0; k < have; k++)
                if (sb[k] == 0) { nul1 = k; break; }
            if (nul1 < 0) return 0;
            int dlen = nul1 < desc_cap - 1 ? nul1 : desc_cap - 1;
            memcpy(desc, sb, (unsigned int)dlen);
            desc[dlen] = '\0';

            // Second string: example / usage.
            int nul2 = -1;
            for (int k = nul1 + 1; k < have; k++)
                if (sb[k] == 0) { nul2 = k; break; }
            int estart = nul1 + 1;
            int elen = (nul2 >= 0 ? nul2 - estart : have - estart);
            if (elen < 0) elen = 0;
            if (elen > example_cap - 1) elen = example_cap - 1;
            memcpy(example, sb + estart, (unsigned int)elen);
            example[elen] = '\0';
            return 1;
        }

        if (used > OVERLAP) {
            memmove(buf, buf + used - OVERLAP, OVERLAP);
            used = OVERLAP;
        }
    }
    return 0;
}

static void usage(void) {
    puts("Usage: metadata <path>\n"
         "Example: metadata /binaries/ping");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    const char* path = argv[1];

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("metadata: not found: %s\n", path);
        return 1;
    }

    int is_dir = fd_is_dir(fd);
    int size   = is_dir ? -1 : file_size_from_fd(fd);

    int blocks = (size > 0) ? ((size + 511) / 512) : 0;

    // Check for ELF magic on regular files. 
    int is_elf = 0;
    char elf_class[16] = "(unknown)";
    if (!is_dir && size >= 4) {
        unsigned char magic[4];
        // Re-open to read from offset 0 cleanly (seek to 0 first).
        eyn_cap_t cap;
        if (eyn_sys_cap_mint_fd(fd, EYN_CAP_R_SEEK, &cap) == 0) {
            eyn_sys_cap_fd_seek(&cap, 0, 0); /* SEEK_SET */
        }
        int nr = read(fd, magic, 4);
        if (nr == 4 && memcmp(magic, ELF_MAGIC, 4) == 0) {
            is_elf = 1;
            // Byte 4 of ELF header: EI_CLASS (1=32-bit, 2=64-bit).
            unsigned char cls = 0;
            if (read(fd, &cls, 1) == 1) {
                if (cls == 1) {
                    memcpy(elf_class, "ELF32", 6);
                } else if (cls == 2) {
                    memcpy(elf_class, "ELF64", 6);
                } else {
                    memcpy(elf_class, "ELF(?)", 7);
                }
            }
        }
    }

    // Scan for embedded ECMD metadata in ELF binaries.
    char ecmd_desc[128]    = "";
    char ecmd_example[128] = "";
    int  has_ecmd = 0;
    if (is_elf) {
        // Seek back to start for the scan.
        eyn_cap_t cap;
        if (eyn_sys_cap_mint_fd(fd, EYN_CAP_R_SEEK, &cap) == 0)
            eyn_sys_cap_fd_seek(&cap, 0, 0);
        has_ecmd = scan_ecmd(fd, ecmd_desc, (int)sizeof(ecmd_desc),
                             ecmd_example, (int)sizeof(ecmd_example));
    }

    close(fd);

    printf("Path:   %s\n", path);
    printf("Name:   %s\n", basename_of(path));

    if (is_dir) {
        printf("Type:   Directory\n");
    } else if (is_elf) {
        printf("Type:   Executable (%s)\n", elf_class);
    } else {
        printf("Type:   File\n");
    }

    if (!is_dir) {
        if (size >= 0) {
            printf("Size:   %d bytes (%d block%s)\n",
                   size, blocks, blocks == 1 ? "" : "s");
        } else {
            printf("Size:   (unavailable)\n");
        }
    }

    if (has_ecmd) {
        printf("Desc:   %s\n", ecmd_desc);
        printf("Usage:  %s\n", ecmd_example);
    }

    return 0;
}
