#ifndef SERIAL_H
#define SERIAL_H

#include <types.h>

// Serial port base addresses
#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8
#define SERIAL_COM3 0x3E8
#define SERIAL_COM4 0x2E8

// Serial port registers
#define SERIAL_DATA_PORT(base)          (base)
#define SERIAL_INTERRUPT_ENABLE(base)   (base + 1)
#define SERIAL_FIFO_CONTROL(base)       (base + 2)
#define SERIAL_LINE_CONTROL(base)       (base + 3)
#define SERIAL_MODEM_CONTROL(base)      (base + 4)
#define SERIAL_LINE_STATUS(base)        (base + 5)
#define SERIAL_MODEM_STATUS(base)       (base + 6)
#define SERIAL_SCRATCH_REGISTER(base)   (base + 7)

// Line control register bits
#define SERIAL_DLAB 0x80  // Divisor latch access bit

// Line status register bits
#define SERIAL_DR   0x01  // Data ready
#define SERIAL_OE   0x02  // Overrun error
#define SERIAL_PE   0x04  // Parity error
#define SERIAL_FE   0x08  // Framing error
#define SERIAL_BI   0x10  // Break indicator
#define SERIAL_THRE 0x20  // Transmitter holding register empty
#define SERIAL_TEMT 0x40  // Transmitter empty
#define SERIAL_IE   0x80  // Impending error

// Modem control register bits
#define SERIAL_DTR 0x01   // Data terminal ready
#define SERIAL_RTS 0x02   // Request to send
#define SERIAL_OUT1 0x04  // Out1
#define SERIAL_OUT2 0x08  // Out2
#define SERIAL_LOOP 0x10  // Loopback mode

// Baud rate divisors (for 115200 baud with 1.8432 MHz clock)
#define SERIAL_BAUD_115200 1
#define SERIAL_BAUD_57600  2
#define SERIAL_BAUD_38400  3
#define SERIAL_BAUD_19200  6
#define SERIAL_BAUD_9600   12
#define SERIAL_BAUD_4800   24
#define SERIAL_BAUD_2400   48
#define SERIAL_BAUD_1200   96

// Serial port configuration
typedef struct {
    uint16 base_port;
    uint32 baud_rate;
    uint8 data_bits;
    uint8 stop_bits;
    uint8 parity;
    uint8 flow_control;
    int initialized;
} serial_port_t;

// Function prototypes
int serial_init(uint16 port, uint32 baud_rate);
void serial_cleanup(uint16 port);
int serial_write(uint16 port, const char* data, int length);
int serial_read(uint16 port, char* buffer, int max_length);
int serial_write_char(uint16 port, char c);
int serial_read_char(uint16 port, char* c);
int serial_is_data_ready(uint16 port);
int serial_is_transmit_empty(uint16 port);
void serial_set_baud_rate(uint16 port, uint32 baud_rate);
void serial_set_data_format(uint16 port, uint8 data_bits, uint8 stop_bits, uint8 parity);

// Global serial ports
extern serial_port_t g_serial_ports[4];

#endif // SERIAL_H
