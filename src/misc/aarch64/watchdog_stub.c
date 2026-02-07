#include <watchdog.h>

void watchdog_init(uint32 timeout_ticks) { (void)timeout_ticks; }
void watchdog_kick(const char* source) { (void)source; }
void watchdog_on_tick(void) { }
void watchdog_set_timeout(uint32 timeout_ticks) { (void)timeout_ticks; }
uint32 watchdog_get_timeout(void) { return 0; }
uint32 watchdog_get_ticks_since_kick(void) { return 0; }
const char* watchdog_get_last_source(void) { return "stub"; }
