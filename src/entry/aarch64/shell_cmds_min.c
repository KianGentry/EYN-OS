#include <misc/types.h>

#include <cpu/aarch64/psci.h>
#include <drivers/aarch64/fb_simple.h>
#include <utilities/shell/shell_command_info.h>

int printf(const char* fmt, ...);

extern volatile uint32 g_aarch64_ticks;

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
REGISTER_SHELL_COMMAND(ticks, "ticks", cmd_ticks, CMD_DIAGNOSTIC,
                       "Show the tick counter.", "ticks");
REGISTER_SHELL_COMMAND(fbinfo, "fbinfo", cmd_fbinfo, CMD_DIAGNOSTIC,
                       "Show framebuffer info.", "fbinfo");
REGISTER_SHELL_COMMAND(meminfo, "meminfo", cmd_meminfo, CMD_DIAGNOSTIC,
                       "Show RAM base/size.", "meminfo");
REGISTER_SHELL_COMMAND(reboot, "reboot", cmd_reboot, CMD_DIAGNOSTIC,
                       "Reboot via PSCI.", "reboot");
REGISTER_SHELL_COMMAND(poweroff, "poweroff", cmd_poweroff, CMD_DIAGNOSTIC,
                       "Power off via PSCI.", "poweroff");
