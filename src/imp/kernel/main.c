#include "print.h"
#include "idt.h"

void kernel_main()
{
	print_str("\nEYN-OS> ");
    kb_init();
    idt_init(); // CAUSES BOOTLOOP, SEE LINE 89 IN 'idt.c'
    while(1);
}