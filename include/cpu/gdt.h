#ifndef GDT_H
#define GDT_H

#include <misc/types.h>

/* GDT selector constants for our installed GDT */
#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_CS   0x1B
#define GDT_USER_DS   0x23
#define GDT_TSS_SEL   0x28

void gdt_init(void);
void tss_set_kernel_stack(uint32 esp0);

/* Enters ring 3 at `entry` with stack pointer `user_stack_top`. Does not return. */
void enter_user_mode(uint32 entry, uint32 user_stack_top);

#endif
