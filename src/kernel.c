#include "../include/kb.h"
#include "../include/isr.h"
#include "../include/idt.h"
#include "../include/util.h"
#include "../include/shell.h"
#include "../include/screen.h"

int kmain()
{
	isr_install();
	clearScreen();
	printf_colored("EYN-OS v0.02\nType 'help' for a list of commands.\n\n", 7, 0);
  	launch_shell(0);
}
