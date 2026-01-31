#include <misc/types.h>
#include <misc/math.h>
#include <misc/printf.h>

#include <cpu/aarch64/psci.h>
#include <drivers/aarch64/fb_simple.h>
#include <utilities/shell/shell_command_info.h>
#include <utilities/shell/fs_commands.h>
#include <fs/vfs.h>

#include <string.h>

extern volatile uint32 g_aarch64_ticks;
extern int shell_redirect_active;

static void write_hex64(uint64 v) {
    static const char* hex = "0123456789ABCDEF";
    printf("0x");
    for (int i = 60; i >= 0; i -= 4) {
        printf("%c", hex[(v >> (uint64)i) & 0xFULL]);
    }
}

static const char* skip_spaces(const char* s) {
    while (s && *s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    return s;
}

static const char* skip_cmd_name(const char* s) {
    while (s && *s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
    return skip_spaces(s);
}

static void cmd_help(string arg) {
    (void)arg;

    uint32 count = (uint32)(__stop_shellcmds - __start_shellcmds);
    printf("Commands (%u):\n", count);

    for (const shell_command_info_t* cmd = __start_shellcmds; cmd < __stop_shellcmds; cmd++) {
        if (!cmd->name) continue;
        printf("  %s", cmd->name);
        if (cmd->description && cmd->description[0]) {
            printf(" - %s", cmd->description);
        }
        printf("\n");
        if (cmd->example && cmd->example[0]) {
            printf("    ex: %s\n", cmd->example);
        }
    }
}

static void cmd_echo(string arg) {
    const char* p = skip_cmd_name(arg);
    if (!p || !*p) {
        printf("\n");
        return;
    }
    printf("%s\n", p);
}

static void cmd_ver(string arg) {
    (void)arg;
    printf("EYN-OS Release 15 (AArch64 port)\n");
}

static void cmd_random(string ch) {
    if (!ch) return;

    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;

    // If no arguments, generate a single random number.
    if (!ch[i]) {
        uint32_t num = rand_next();
        if (shell_redirect_active) {
            printf("%d\n", (int)num);
        } else {
            printf("%cRandom number: %d\n", 255, 255, 255, (int)num);
        }
        return;
    }

    // Parse first argument (count or min)
    if (ch[i] >= '0' && ch[i] <= '9') {
        uint32_t arg1 = 0;
        while (ch[i] >= '0' && ch[i] <= '9') {
            if (arg1 > UINT32_MAX / 10) {
                printf("%cError: Number too large\n", 255, 0, 0);
                return;
            }
            arg1 = arg1 * 10 + (uint32_t)(ch[i] - '0');
            i++;
        }

        while (ch[i] && ch[i] == ' ') i++;

        // Check if there's a second argument
        if (ch[i] && ch[i] >= '0' && ch[i] <= '9') {
            uint32_t arg2 = 0;
            while (ch[i] && ch[i] >= '0' && ch[i] <= '9') {
                if (arg2 > UINT32_MAX / 10) {
                    printf("%cError: Number too large\n", 255, 0, 0);
                    return;
                }
                arg2 = arg2 * 10 + (uint32_t)(ch[i] - '0');
                i++;
            }

            if (arg1 >= arg2) {
                printf("%cError: min must be less than max\n", 255, 0, 0);
                return;
            }
            if (arg2 - arg1 > 1000) {
                printf("%cError: Range too large (max 1000)\n", 255, 0, 0);
                return;
            }

            uint32_t num = rand_range(arg1, arg2);
            if (shell_redirect_active) {
                printf("%d\n", (int)num);
            } else {
                printf("%cRandom number in range [%d, %d]: %d\n", 255, 255, 255, (int)arg1, (int)arg2, (int)num);
            }
        } else {
            // Single arg: treat as count.
            if (arg1 > 1000) {
                printf("%cError: Count too large (max 1000)\n", 255, 0, 0);
                return;
            }

            if (shell_redirect_active) {
                for (uint32_t k = 0; k < arg1; k++) {
                    uint32_t num = rand_next();
                    printf("%d", (int)num);
                    if (k < arg1 - 1) printf(" ");
                }
                printf("\n");
            } else {
                printf("%cGenerating %d random numbers:\n", 255, 255, 255, (int)arg1);
                for (uint32_t k = 0; k < arg1; k++) {
                    uint32_t num = rand_next();
                    printf("%c%d", 255, 255, 255, (int)num);
                    if (k < arg1 - 1) printf(", ");
                    if ((k + 1) % 10 == 0) printf("\n");
                }
                printf("\n");
            }
        }
    } else {
        printf("%cError: Invalid number format\n", 255, 0, 0);
        return;
    }
}

static void cmd_calc(string ch) {
    const char* expr = skip_cmd_name(ch);
    if (!expr || !*expr) {
        printf("%cUsage: calc <expression>\n", 255, 255, 255);
        printf("%cExample: calc 2.5+3.7\n", 255, 255, 255);
        return;
    }

    int32_t v = math_get_current_equation((string)expr);
    // math_get_current_equation returns fixed-point (3 decimals).
    int32_t int_part = v / FIXED_POINT_FACTOR;
    int32_t frac = v % FIXED_POINT_FACTOR;
    if (frac < 0) frac = -frac;
    printf("%d.%03d\n", (int)int_part, (int)frac);
}

static void cmd_sort(string ch) {
    const char* args = skip_cmd_name(ch);
    if (!args || !*args) {
        printf("%cUsage: sort <string1> <string2> ...\n", 255, 255, 255);
        printf("%cExample: sort zebra apple banana\n", 255, 255, 255);
        return;
    }

    char buf[512];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* argv[32];
    int argc = 0;

    char* save = NULL;
    char* tok = strtok_r(buf, " \t\r\n", &save);
    while (tok && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &save);
    }

    if (argc <= 0) {
        return;
    }

    quicksort_strings(argv, 0, argc - 1);

    for (int i = 0; i < argc; i++) {
        printf("%s", argv[i]);
        if (i + 1 < argc) printf(" ");
    }
    printf("\n");
}

static void cmd_ticks(string arg) {
    (void)arg;
    printf("ticks ");
    write_hex64((uint64)g_aarch64_ticks);
    printf("\n");
}

static void cmd_fbinfo(string arg) {
    (void)arg;
    if (!fb_simple_ready()) {
        printf("fb not ready\n");
        return;
    }

    uint64 base = 0;
    uint32 w = 0, h = 0, stride = 0, bpp = 0;
    const char* fmt = 0;
    if (fb_simple_get_info(&base, &w, &h, &stride, &bpp, &fmt) != 0) {
        printf("fb info unavailable\n");
        return;
    }

    printf("fb base "); write_hex64(base);
    printf(" w "); write_hex64((uint64)w);
    printf(" h "); write_hex64((uint64)h);
    printf(" stride "); write_hex64((uint64)stride);
    printf(" bpp "); write_hex64((uint64)bpp);
    printf(" fmt %s\n", fmt ? fmt : "(null)");
}

static void cmd_vfsdetect(string arg) {
    (void)arg;
    vfs_fs_type_t t = vfs_detect(0);
    switch (t) {
        case VFS_FS_EYNFS: printf("vfs: EYNFS\n"); break;
        case VFS_FS_FAT32: printf("vfs: FAT32\n"); break;
        default: printf("vfs: none\n"); break;
    }
}

static int ls_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)user;
    if (!name) return 0;
    if (is_dir) {
        printf("%s/\n", name);
    } else {
        printf("%s (%u)\n", name, (unsigned)size);
    }
    return 0;
}

static void cmd_ls(string arg) {
    const char* path = skip_cmd_name(arg);
    if (!path || !*path) path = "/";

    char abspath[128];
    resolve_path(path, shell_current_path, abspath, sizeof(abspath));
    int rc = vfs_listdir(0, abspath, ls_cb, 0);
    if (rc != 0) {
        printf("%cError: listdir failed (%d)\n", 255, 0, 0, rc);
    }
}

static void cmd_cat(string arg) {
    const char* path = skip_cmd_name(arg);
    if (!path || !*path) {
        printf("%cUsage: cat <path>\n", 255, 255, 255);
        printf("%cExample: cat /test.txt\n", 255, 255, 255);
        return;
    }

    char abspath[128];
    resolve_path(path, shell_current_path, abspath, sizeof(abspath));

    uint32 offset = 0;
    char buf[257];
    for (;;) {
        int n = vfs_read_file_at(0, abspath, buf, 256, offset);
        if (n < 0) {
            printf("%cError: read failed (%d)\n", 255, 0, 0, n);
            return;
        }
        if (n == 0) break;
        buf[n] = '\0';
        printf("%s", buf);
        offset += (uint32)n;
        if (n < 256) break;
    }
    printf("\n");
}

static uint64 g_ram_base;
static uint64 g_ram_size;

void aarch64_shell_set_meminfo(uint64 ram_base, uint64 ram_size) {
    g_ram_base = ram_base;
    g_ram_size = ram_size;
}

static void cmd_meminfo(string arg) {
    (void)arg;
    printf("ram base "); write_hex64(g_ram_base);
    printf(" size "); write_hex64(g_ram_size);
    printf("\n");
}

static void cmd_reboot(string arg) {
    (void)arg;
    printf("rebooting (psci system_reset)...\n");
    psci_system_reset();
}

static void cmd_poweroff(string arg) {
    (void)arg;
    printf("powering off (psci system_off)...\n");
    psci_system_off();
}

REGISTER_SHELL_COMMAND(help, "help", cmd_help, CMD_ESSENTIAL,
                       "List available commands.", "help");
REGISTER_SHELL_COMMAND(echo, "echo", cmd_echo, CMD_ESSENTIAL,
                       "Print arguments.", "echo hello");
REGISTER_SHELL_COMMAND(ver, "ver", cmd_ver, CMD_ESSENTIAL,
                       "Show version information.", "ver");
REGISTER_SHELL_COMMAND(calc, "calc", cmd_calc, CMD_STREAMING,
                       "32-bit fixed-point calculator. Supports +, -, *, /.", "calc 2.5+3.7");
REGISTER_SHELL_COMMAND(random, "random", cmd_random, CMD_STREAMING,
                       "Generate random numbers.\nUsage: random [count] | random [min] [max]", "random 5");
REGISTER_SHELL_COMMAND(sort, "sort", cmd_sort, CMD_STREAMING,
                       "Sort strings alphabetically.", "sort zebra apple banana");
REGISTER_SHELL_COMMAND(ticks, "ticks", cmd_ticks, CMD_DIAGNOSTIC,
                       "Show the tick counter.", "ticks");
REGISTER_SHELL_COMMAND(fbinfo, "fbinfo", cmd_fbinfo, CMD_DIAGNOSTIC,
                       "Show framebuffer info.", "fbinfo");
REGISTER_SHELL_COMMAND(vfsdetect, "vfsdetect", cmd_vfsdetect, CMD_DIAGNOSTIC,
                       "Detect filesystem on drive 0.", "vfsdetect");
REGISTER_SHELL_COMMAND(ls, "ls", cmd_ls, CMD_STREAMING,
                       "List a directory (drive 0).", "ls /");
REGISTER_SHELL_COMMAND(cat, "cat", cmd_cat, CMD_STREAMING,
                       "Print a text file (drive 0).", "cat /test.txt");
REGISTER_SHELL_COMMAND(meminfo, "meminfo", cmd_meminfo, CMD_DIAGNOSTIC,
                       "Show RAM base/size.", "meminfo");
REGISTER_SHELL_COMMAND(reboot, "reboot", cmd_reboot, CMD_DIAGNOSTIC,
                       "Reboot via PSCI.", "reboot");
REGISTER_SHELL_COMMAND(poweroff, "poweroff", cmd_poweroff, CMD_DIAGNOSTIC,
                       "Power off via PSCI.", "poweroff");
