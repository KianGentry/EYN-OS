#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("List directory entries with terminal icons.", "list [path]");

#define LIST_MAX_ENTRIES 256

typedef struct {
    char name[56];
    uint8_t is_dir;
} list_entry_t;

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
    puts("Usage: list [path]\nExamples:\n  list\n  list /");
}

int main(int argc, char** argv) {
    const char* path = "";
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
        printf("list: failed to open: %s\n", (path && path[0]) ? path : ".");
        console_set_rgb(255, 255, 255);
        return 1;
    }

    eyn_dirent_t ents[16];
    list_entry_t entries[LIST_MAX_ENTRIES];
    int entry_count = 0;
    for (;;) {
        int rc = getdents(fd, ents, sizeof(ents));
        if (rc < 0) {
            console_set_rgb(255, 0, 0);
            puts("list: getdents error");
            console_set_rgb(255, 255, 255);
            break;
        }
        if (rc == 0) break;
        int count = rc / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            if (ents[i].name[0] == '\0') continue;
            if (entry_count >= LIST_MAX_ENTRIES) continue;

            int n = 0;
            for (; n < (int)sizeof(entries[entry_count].name) - 1 && ents[i].name[n]; ++n)
                entries[entry_count].name[n] = ents[i].name[n];
            entries[entry_count].name[n] = '\0';
            entries[entry_count].is_dir = ents[i].is_dir ? 1 : 0;
            entry_count++;
        }
    }

    for (int i = 1; i < entry_count; ++i) {
        list_entry_t key = entries[i];
        int j = i - 1;
        while (j >= 0 && strcmp(entries[j].name, key.name) > 0) {
            entries[j + 1] = entries[j];
            --j;
        }
        entries[j + 1] = key;
    }

    for (int i = 0; i < entry_count; ++i) {
        console_set_icon_key(icon_key_for_entry(entries[i].name, entries[i].is_dir));

        if (entries[i].is_dir) {
            console_set_rgb(120, 120, 255);
            printf("  %s/\n", entries[i].name);
        } else {
            console_set_rgb(255, 255, 255);
            printf("  %s\n", entries[i].name);
        }
    }

    (void)close(fd);
    console_set_rgb(255, 255, 255);
    return 0;
}
