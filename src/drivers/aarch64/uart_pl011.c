#include <misc/types.h>

/*
 * PL011 UART base addresses by platform.
 *
 * - Raspberry Pi 4 (BCM2711): UART0 @ 0xFE201000
 * - QEMU 'virt': PL011 @ 0x09000000
 */
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
#define UART0_BASE 0x09000000ull
#elif defined(AARCH64_PLATFORM_RPI4)
#define UART0_BASE 0xFE201000ull
#else
/* Default to Raspberry Pi 4 when the build system didn't provide a platform. */
#define UART0_BASE 0xFE201000ull
#endif

#define UART_DR            0x00
#define UART_FR            0x18
#define UART_IBRD          0x24
#define UART_FBRD          0x28
#define UART_LCRH          0x2C
#define UART_CR            0x30
#define UART_IMSC          0x38
#define UART_ICR           0x44

#define UART_FR_TXFF       (1u << 5)
#define UART_FR_RXFE       (1u << 4)

/*
 * Simple UART output lock.
 *
 * QEMU virt SMP brings up multiple cores early; without a lock, concurrent
 * UART writes interleave and make logs unreadable.
 */
static volatile uint32 g_uart_tx_lock = 0;

static inline void uart_tx_lock(void) {
    for (;;) {
        uint32 val;
        uint32 st;

        /* Load-acquire current lock value. */
        asm volatile("ldaxr %w0, [%1]" : "=&r"(val) : "r"(&g_uart_tx_lock) : "memory");
        if (val != 0) {
            /* Someone holds the lock: wait for event then retry. */
            asm volatile("wfe" ::: "memory");
            continue;
        }

        /* Try to store 1; st=0 on success. */
        asm volatile("stxr %w0, %w2, [%1]" : "=&r"(st) : "r"(&g_uart_tx_lock), "r"(1u) : "memory");
        if (st == 0) {
            return;
        }
    }
}

static inline void uart_tx_unlock(void) {
    /* Store-release 0 and signal waiters. */
    asm volatile("stlr %w0, [%1]" :: "r"(0u), "r"(&g_uart_tx_lock) : "memory");
    asm volatile("sev" ::: "memory");
}

static inline void mmio_write(uint64 reg, uint32 value) {
    *(volatile uint32*)reg = value;
}

static inline uint32 mmio_read(uint64 reg) {
    return *(volatile uint32*)reg;
}

static void uart_pl011_putc_unlocked(char c) {
    // Wait for space in the TX FIFO.
    while (mmio_read(UART0_BASE + UART_FR) & UART_FR_TXFF) {
        asm volatile("yield" ::: "memory");
    }

    mmio_write(UART0_BASE + UART_DR, (uint32)c);
}

void uart_pl011_init_115200(void) {
    // Disable UART.
    mmio_write(UART0_BASE + UART_CR, 0);

    // Clear interrupts.
    mmio_write(UART0_BASE + UART_ICR, 0x7FF);

    /*
     * Baud rate divisors.
     *
     * RPi4 firmware typically provides UART0 clock at 48MHz.
     * QEMU 'virt' PL011 uses a 24MHz reference clock.
     */
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
    /* 24_000_000 / (16 * 115200) = 13.0208 -> IBRD=13, FBRD=1 */
    mmio_write(UART0_BASE + UART_IBRD, 13);
    mmio_write(UART0_BASE + UART_FBRD, 1);
#else
    /* 48_000_000 / (16 * 115200) = 26.0417 -> IBRD=26, FBRD=3 */
    mmio_write(UART0_BASE + UART_IBRD, 26);
    mmio_write(UART0_BASE + UART_FBRD, 3);
#endif

    // 8N1, enable FIFOs.
    mmio_write(UART0_BASE + UART_LCRH, (3u << 5) | (1u << 4));

    // Mask all interrupts (polling mode for early bring-up).
    mmio_write(UART0_BASE + UART_IMSC, 0);

    // Enable UART, TX, RX.
    mmio_write(UART0_BASE + UART_CR, (1u << 0) | (1u << 8) | (1u << 9));
}

void uart_pl011_putc(char c) {
    uart_tx_lock();
    uart_pl011_putc_unlocked(c);
    uart_tx_unlock();
}

void uart_pl011_write(const char* s) {
    if (!s) return;

    uart_tx_lock();
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            uart_pl011_putc_unlocked('\r');
        }
        uart_pl011_putc_unlocked(c);
    }
    uart_tx_unlock();
}

void uart_pl011_write_hex64(uint64 v) {
    static const char* hex = "0123456789ABCDEF";

    uart_tx_lock();
    uart_pl011_putc_unlocked('0');
    uart_pl011_putc_unlocked('x');
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex[(v >> (uint64)i) & 0xFULL];
        uart_pl011_putc_unlocked(c);
    }
    uart_tx_unlock();
}

/*
 * Non-blocking receive.
 * Returns 0 on success (writes received char to *out_c), non-zero if no data.
 */
int uart_pl011_getc_nonblock(char* out_c) {
    if (!out_c) return -1;
    if (mmio_read(UART0_BASE + UART_FR) & UART_FR_RXFE) {
        return 1;
    }

    uint32 v = mmio_read(UART0_BASE + UART_DR);
    *out_c = (char)(v & 0xFFu);
    return 0;
}
