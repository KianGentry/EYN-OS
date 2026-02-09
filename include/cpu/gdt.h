#ifndef GDT_H
#define GDT_H

#include <types.h>

/* GDT selector constants for our installed GDT */
#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_CS   0x1B
#define GDT_USER_DS   0x23
#define GDT_TSS_SEL   0x28
#define GDT_LDT_SEL   0x30

void gdt_init(void);
void tss_set_kernel_stack(uint32 esp0);
void gdt_set_ldt_descriptor(uint32 base, uint32 limit);

/* Enters ring 3 at `entry` with stack pointer `user_stack_top`. Does not return. */
void enter_user_mode(uint32 entry, uint32 user_stack_top);

/* Enters ring 3 using provided selectors (typically LDT-based). Does not return. */
void enter_user_mode_segdom(uint32 entry, uint32 user_stack_top, uint16 user_cs, uint16 user_ds);

#endif
