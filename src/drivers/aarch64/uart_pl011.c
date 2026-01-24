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

static inline void mmio_write(uint64 reg, uint32 value) {
    *(volatile uint32*)reg = value;
}

static inline uint32 mmio_read(uint64 reg) {
    return *(volatile uint32*)reg;
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
    // Wait for space in the TX FIFO.
    while (mmio_read(UART0_BASE + UART_FR) & UART_FR_TXFF) {
        asm volatile("yield" ::: "memory");
    }

    mmio_write(UART0_BASE + UART_DR, (uint32)c);
}

void uart_pl011_write(const char* s) {
    if (!s) return;

    while (*s) {
        char c = *s++;
        if (c == '\n') {
            uart_pl011_putc('\r');
        }
        uart_pl011_putc(c);
    }
}
