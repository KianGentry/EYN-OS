#include <context.h>

command_context_t* current_command_context = 0;

void command_context_set(command_context_t* ctx) {
    current_command_context = ctx;
}

void command_context_clear(void) {
    current_command_context = 0;
}

command_context_t* command_context_get(void) {
    return current_command_context;
}
