#include <util.h>

// AArch64 builds currently do not link the full i386 util/tile manager stack,
// but several shared components expect these global flags to exist.
// Keep them here as minimal definitions.

volatile int g_user_interrupt = 0;
volatile int g_user_task_active = 0;
volatile int g_abort_to_shell = 0;
volatile int g_user_task_term = -1;
volatile int g_user_task_ui_dirty = 0;

volatile uint32 g_user_code_base = 0;
volatile uint32 g_user_code_pages = 0;
volatile uint32 g_user_stack_page = 0;
