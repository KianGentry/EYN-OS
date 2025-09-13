#include <serial.h>
#include <system.h>
#include <vga.h>
#include <string.h>

// Global serial ports
serial_port_t g_serial_ports[4] = {
    {SERIAL_COM1, 9600, 8, 1, 0, 0, 0},
    {SERIAL_COM2, 9600, 8, 1, 0, 0, 0},
    {SERIAL_COM3, 9600, 8, 1, 0, 0, 0},
    {SERIAL_COM4, 9600, 8, 1, 0, 0, 0}
};

// Get serial port index from base address
static int get_port_index(uint16 base_port) {
    switch (base_port) {
        case SERIAL_COM1: return 0;
        case SERIAL_COM2: return 1;
        case SERIAL_COM3: return 2;
        case SERIAL_COM4: return 3;
        default: return -1;
    }
}

// Wait for serial port to be ready
static int serial_wait_for_ready(uint16 port) {
    int timeout = 100000;
    while (timeout--) {
        if (inportb(SERIAL_LINE_STATUS(port)) & SERIAL_THRE) {
            return 0; // Ready
        }
    }
    return -1; // Timeout
}

// Wait for data to be available
static int serial_wait_for_data(uint16 port) {
    int timeout = 100000;
    while (timeout--) {
        if (inportb(SERIAL_LINE_STATUS(port)) & SERIAL_DR) {
            return 0; // Data available
        }
    }
    return -1; // Timeout
}

// Initialize serial port
int serial_init(uint16 port, uint32 baud_rate) {
    int port_index = get_port_index(port);
    if (port_index == -1) return -1;
    
    serial_port_t* serial = &g_serial_ports[port_index];
    
    // Disable interrupts
    outportb(SERIAL_INTERRUPT_ENABLE(port), 0x00);
    
    // Enable DLAB (divisor latch access bit)
    outportb(SERIAL_LINE_CONTROL(port), SERIAL_DLAB);
    
    // Set divisor for baud rate
    uint16 divisor = 115200 / baud_rate;
    outportb(SERIAL_DATA_PORT(port), divisor & 0xFF);
    outportb(SERIAL_DATA_PORT(port), (divisor >> 8) & 0xFF);
    
    // Set data format: 8 data bits, 1 stop bit, no parity
    outportb(SERIAL_LINE_CONTROL(port), 0x03);
    
    // Enable FIFO, clear them, with 14-byte threshold
    outportb(SERIAL_FIFO_CONTROL(port), 0xC7);
    
    // Enable IRQs, RTS/DSR set
    outportb(SERIAL_MODEM_CONTROL(port), 0x0B);
    
    // Test serial chip (send byte 0xAE and check if it comes back)
    outportb(SERIAL_MODEM_CONTROL(port), 0x1E); // Set in loopback mode
    outportb(SERIAL_DATA_PORT(port), 0xAE);     // Send test byte
    
    // Check if we can read it back
    if (inportb(SERIAL_DATA_PORT(port)) != 0xAE) {
        printf("Serial port 0x%04X test failed\n", port);
        return -1;
    }
    
    // Set normal operation mode
    outportb(SERIAL_MODEM_CONTROL(port), 0x0F);
    
    // Update port configuration
    serial->base_port = port;
    serial->baud_rate = baud_rate;
    serial->initialized = 1;
    
    printf("Serial port 0x%04X initialized at %d baud\n", port, baud_rate);
    return 0;
}

// Cleanup serial port
void serial_cleanup(uint16 port) {
    int port_index = get_port_index(port);
    if (port_index == -1) return;
    
    serial_port_t* serial = &g_serial_ports[port_index];
    
    // Disable interrupts
    outportb(SERIAL_INTERRUPT_ENABLE(port), 0x00);
    
    // Disable DTR and RTS
    outportb(SERIAL_MODEM_CONTROL(port), 0x00);
    
    serial->initialized = 0;
    printf("Serial port 0x%04X cleaned up\n", port);
}

// Write data to serial port
int serial_write(uint16 port, const char* data, int length) {
    if (!data || length <= 0) return -1;
    
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return -1;
    
    int written = 0;
    for (int i = 0; i < length; i++) {
        if (serial_write_char(port, data[i]) == 0) {
            written++;
        } else {
            break;
        }
    }
    
    return written;
}

// Read data from serial port
int serial_read(uint16 port, char* buffer, int max_length) {
    if (!buffer || max_length <= 0) return -1;
    
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return -1;
    
    int read = 0;
    for (int i = 0; i < max_length; i++) {
        char c;
        if (serial_read_char(port, &c) == 0) {
            buffer[i] = c;
            read++;
        } else {
            break;
        }
    }
    
    return read;
}

// Write single character to serial port
int serial_write_char(uint16 port, char c) {
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return -1;
    
    // Wait for transmitter to be ready
    if (serial_wait_for_ready(port) != 0) return -1;
    
    // Send character
    outportb(SERIAL_DATA_PORT(port), c);
    
    return 0;
}

// Read single character from serial port
int serial_read_char(uint16 port, char* c) {
    if (!c) return -1;
    
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return -1;
    
    // Check if data is available
    if (!(inportb(SERIAL_LINE_STATUS(port)) & SERIAL_DR)) {
        return -1; // No data available
    }
    
    // Read character
    *c = inportb(SERIAL_DATA_PORT(port));
    
    return 0;
}

// Check if data is ready to be read
int serial_is_data_ready(uint16 port) {
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return 0;
    
    return (inportb(SERIAL_LINE_STATUS(port)) & SERIAL_DR) ? 1 : 0;
}

// Check if transmitter is empty
int serial_is_transmit_empty(uint16 port) {
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return 0;
    
    return (inportb(SERIAL_LINE_STATUS(port)) & SERIAL_THRE) ? 1 : 0;
}

// Set baud rate
void serial_set_baud_rate(uint16 port, uint32 baud_rate) {
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return;
    
    // Enable DLAB
    outportb(SERIAL_LINE_CONTROL(port), SERIAL_DLAB);
    
    // Set divisor
    uint16 divisor = 115200 / baud_rate;
    outportb(SERIAL_DATA_PORT(port), divisor & 0xFF);
    outportb(SERIAL_DATA_PORT(port), (divisor >> 8) & 0xFF);
    
    // Disable DLAB
    outportb(SERIAL_LINE_CONTROL(port), 0x03);
    
    g_serial_ports[port_index].baud_rate = baud_rate;
}

// Set data format
void serial_set_data_format(uint16 port, uint8 data_bits, uint8 stop_bits, uint8 parity) {
    int port_index = get_port_index(port);
    if (port_index == -1 || !g_serial_ports[port_index].initialized) return;
    
    uint8 format = 0;
    
    // Data bits (5-8)
    if (data_bits >= 5 && data_bits <= 8) {
        format |= (data_bits - 5);
    } else {
        format |= 0x03; // Default to 8 bits
    }
    
    // Stop bits
    if (stop_bits == 2) {
        format |= 0x04;
    }
    
    // Parity
    if (parity) {
        format |= 0x08;
        if (parity == 2) { // Even parity
            format |= 0x10;
        }
    }
    
    outportb(SERIAL_LINE_CONTROL(port), format);
    
    g_serial_ports[port_index].data_bits = data_bits;
    g_serial_ports[port_index].stop_bits = stop_bits;
    g_serial_ports[port_index].parity = parity;
}
