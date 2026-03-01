#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <string.h>

#include <eynos_cmdmeta.h>

// Optional help metadata consumed by the kernel's `help` command.
// This is stored in an ELF section named `.eynos.cmdmeta`.
EYN_CMDMETA_V1("List directory entries.", "ls");

static void console_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    unsigned char seq[4];
    seq[0] = 0xFF;
    seq[1] = (unsigned char)r;
    seq[2] = (unsigned char)g;
    seq[3] = (unsigned char)b;
    (void)write(1, seq, 4);
}

static void console_set_icon_key(const char* key) {
    unsigned char seq[1 + 16];
    seq[0] = 0xFE;
    for (int i = 0; i < 16; ++i) seq[1 + i] = 0;
    if (key && key[0]) {
        size_t n = strlen(key);
        if (n > 15) n = 15;
        memcpy(&seq[1], key, n);
    }
    (void)write(1, seq, sizeof(seq));
}

static const char* icon_key_for_entry(const char* name, int is_dir) {
    if (is_dir) return "dir_full";
    if (!name) return "file_none";

    const char* dot = strrchr(name, '.');
    if (!dot || dot == name) return "file_none";

    if (strcmp(dot, ".txt") == 0) return "file_txt";
    if (strcmp(dot, ".md") == 0) return "file_md";
    if (strcmp(dot, ".rei") == 0) return "file_rei";
    if (strcmp(dot, ".reiv") == 0) return "file_rei";
    if (strcmp(dot, ".c") == 0) return "file_c";
    if (strcmp(dot, ".h") == 0) return "file_c";
    if (strcmp(dot, ".asm") == 0) return "file_asm";
    if (strcmp(dot, ".s") == 0) return "file_asm";
    if (strcmp(dot, ".sh") == 0) return "file_shell";
    if (strcmp(dot, ".uelf") == 0) return "file_bin";
    if (strcmp(dot, ".eyn") == 0) return "file_eyn";
    return "file_none";
}

static void usage(void) {
    puts("Usage: ls [path]\nExamples:\n  ls\n  ls /");
}

int main(int argc, char** argv) {
    const char* path = ""; // empty path resolves to CWD in the kernel
    if (argc >= 2) {
        if (argv[1][0] == '-' && argv[1][1] == 'h' && argv[1][2] == '\0') {
            usage();
            return 0;
        }
        if (argv[1][0] == '\0') {
            usage();
            return 1;
        }
        path = argv[1];
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        console_set_rgb(255, 0, 0);
        printf("ls: failed to open: %s\n", (path && path[0]) ? path : ".");
        console_set_rgb(255, 255, 255);
        return 1;
    }

    eyn_dirent_t ents[16];
    for (;;) {
        int rc = getdents(fd, ents, sizeof(ents));
        if (rc < 0) {
            console_set_rgb(255, 0, 0);
            puts("ls: getdents error");
            console_set_rgb(255, 255, 255);
            break;
        }
        if (rc == 0) break;
        int count = rc / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            if (ents[i].name[0] == '\0') continue;

            // Attach an icon marker to this output line before printing any text.
            console_set_icon_key(icon_key_for_entry(ents[i].name, ents[i].is_dir));

            if (ents[i].is_dir) {
                console_set_rgb(120, 120, 255);
                printf("  %s/\n", ents[i].name);
            } else {
                console_set_rgb(255, 255, 255);
                printf("  %s\n", ents[i].name);
            }
        }
    }

    (void)close(fd);
    console_set_rgb(255, 255, 255);
    return 0;
}
