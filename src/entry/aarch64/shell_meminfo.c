#include <misc/types.h>

static uint64 g_ram_base;
static uint64 g_ram_size;

void aarch64_shell_set_meminfo(uint64 ram_base, uint64 ram_size)
{
    g_ram_base = ram_base;
    g_ram_size = ram_size;
}

// Optional helper for future commands/debugging.
uint64 aarch64_shell_get_ram_base(void)
{
    return g_ram_base;
}

uint64 aarch64_shell_get_ram_size(void)
{
    return g_ram_size;
}
