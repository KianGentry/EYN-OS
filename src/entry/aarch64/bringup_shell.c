#include <misc/types.h>
#include <drivers/aarch64/fb_simple.h>

/*
 * AArch64 bring-up shell
 *
 * This is a minimal interactive console used while porting the i386 kernel to
 * AArch64. It intentionally avoids dependencies on VGA/keyboard/ATA/VFS.
 *
 * It is not meant to replace the existing EYN-OS shell; it provides a stable
 * interactive surface so we can port subsystems incrementally.
 */

void uart_pl011_putc(char c);
void uart_pl011_write(const char* s);
int uart_pl011_getc_nonblock(char* out_c);
int virtio_input_getc_nonblock(char* out_c);

int aarch64_shell_dispatch_line(string line);
void aarch64_shell_set_meminfo(uint64 ram_base, uint64 ram_size);

extern volatile uint32 g_aarch64_ticks;

static void console_putc(char c) {
    if (c == '\n') {
        uart_pl011_putc('\r');
    }
    uart_pl011_putc(c);

    if (fb_simple_ready()) {
        if (c == '\n') fb_simple_putc('\r');
        fb_simple_putc(c);
    }
}

static void console_write(const char* s) {
    if (!s) return;
    while (*s) console_putc(*s++);
}

static int is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static const char* skip_spaces(const char* s) {
    while (s && *s && is_space(*s)) s++;
    return s;
}

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void read_line(char* buf, int cap) {
    if (!buf || cap <= 0) return;

    int n = 0;
    buf[0] = '\0';

    for (;;) {
        char c;
        for (;;) {
            if (uart_pl011_getc_nonblock(&c) == 0) {
                break;
            }
            if (virtio_input_getc_nonblock(&c) == 0) {
                break;
            }
            /* Idle until next interrupt (timer) then poll again. */
            asm volatile("wfi" ::: "memory");
        }

        if (c == '\r' || c == '\n') {
            console_putc('\n');
            break;
        }

        /* Backspace / DEL */
        if (c == '\b' || c == 0x7F) {
            if (n > 0) {
                n--;
                buf[n] = '\0';
                /* Erase char on terminal. */
                console_write("\b \b");
            }
            continue;
        }

        if (c < 32 || c > 126) {
            continue;
        }

        if (n < cap - 1) {
            buf[n++] = c;
            buf[n] = '\0';
            console_putc(c);
        }
    }
}

void aarch64_bringup_shell_set_meminfo(uint64 ram_base, uint64 ram_size) {
    aarch64_shell_set_meminfo(ram_base, ram_size);
}

void aarch64_bringup_shell_run(void) {
    console_write("\nEYN-OS AArch64 shell ready. Type 'help'.\n\n");

    for (;;) {
        char line[200] __attribute__((aligned(16)));
        console_write("a64> ");
        read_line(line, (int)sizeof(line));

        const char* cmd = skip_spaces(line);
        if (!cmd || *cmd == '\0') {
            continue;
        }

        int rc = aarch64_shell_dispatch_line(line);
        if (rc == -1) {
            /* Unknown: keep behavior similar to classic shell. */
            console_write("unknown command: ");
            /* Print command name only */
            char name[32] __attribute__((aligned(16)));
            int i = 0;
            while (cmd[i] && !is_space(cmd[i]) && i < (int)sizeof(name) - 1) {
                name[i] = cmd[i];
                i++;
            }
            name[i] = '\0';
            console_write(name);
            console_putc('\n');
        }
    }
}
