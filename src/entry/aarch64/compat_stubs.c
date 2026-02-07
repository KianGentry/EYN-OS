#include <misc/types.h>
#include <misc/printf.h>
#include <drivers/serial.h>
#include <hal/console.h>

int serial_write(uint16 port, const char* data, int length) {
    (void)port;
    if (!data || length <= 0) return 0;
    hal_console_write_len(data, (uint32)length);
    return length;
}

int serial_write_char(uint16 port, char c) {
    (void)port;
    hal_console_putc(c);
    return 1;
}

uint32 get_heap_size(void) { return 0; }
uint32 get_heap_used(void) { return 0; }
uint32 get_total_ram(void) { return 0; }

void print_memory_stats(void) {
    printf("Memory stats not available on this build.\n");
}

void check_stack_overflow(void) { }
int get_memory_error_count(void) { return 0; }
int get_stack_overflow_status(void) { return 0; }
uint32 get_current_stack_pointer(void) { return 0; }

uint32 sched_get_idle_hlt_count(void) { return 0; }
