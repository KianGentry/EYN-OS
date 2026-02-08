#include <misc/types.h>
#include <misc/printf.h>
#include <drivers/serial.h>
#include <hal/console.h>
#include <network/netstack.h>

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

__attribute__((weak)) uint32 sched_get_idle_hlt_count(void) { return 0; }

__attribute__((weak)) int kstdin_get_line(const char** out_s, int* out_len) {
    (void)out_s;
    (void)out_len;
    return 0;
}

__attribute__((weak)) void kstdin_consume_line(void) { }

__attribute__((weak)) void net_get_local_ip(uint8 out_ip[4]) {
    if (out_ip) {
        out_ip[0] = 0;
        out_ip[1] = 0;
        out_ip[2] = 0;
        out_ip[3] = 0;
    }
}

__attribute__((weak)) int net_udp_bind(uint16 port) {
    (void)port;
    return -1;
}

__attribute__((weak)) int net_udp_close(int socket_id) {
    (void)socket_id;
    return -1;
}

__attribute__((weak)) int net_udp_send_socket(int socket_id, const uint8 src_ip[4],
                                              const uint8 dst_ip[4], uint16 dst_port,
                                              const uint8* payload, uint32 payload_len,
                                              int arp_spins) {
    (void)socket_id;
    (void)src_ip;
    (void)dst_ip;
    (void)dst_port;
    (void)payload;
    (void)payload_len;
    (void)arp_spins;
    return -1;
}

__attribute__((weak)) int net_udp_recv_socket(int socket_id, net_udp_rx_packet* out) {
    (void)socket_id;
    if (out) {
        out->payload_len = 0;
    }
    return 0;
}
