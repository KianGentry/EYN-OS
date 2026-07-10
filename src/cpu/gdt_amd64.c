#include <gdt.h>
#include <string.h>

typedef struct {
    uint16 limit;
    uint64 base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct {
    uint32 reserved0;
    uint64 rsp0;
    uint64 rsp1;
    uint64 rsp2;
    uint64 reserved1;
    uint64 ist1;
    uint64 ist2;
    uint64 ist3;
    uint64 ist4;
    uint64 ist5;
    uint64 ist6;
    uint64 ist7;
    uint64 reserved2;
    uint16 reserved3;
    uint16 iomap_base;
} __attribute__((packed)) tss_entry64_t;

/*
 * ABI-INVARIANT: GDT slot assignments must stay stable.
 *
 * Why: Selector constants in include/cpu/gdt.h are consumed by ISR/syscall
 * entry stubs and user-mode transitions.
 * Breakage if changed: Wrong selectors at iretq/ltr time produce #GP or
 * triple-fault during privilege transitions.
 */
#define GDT_ENTRY_COUNT 8u
#define GDT_INDEX_TSS_LO 5u
#define GDT_INDEX_TSS_HI 6u

static uint64 gdt_entries[GDT_ENTRY_COUNT];
static gdt_ptr_t gdt_ptr;
static tss_entry64_t g_tss;

extern void gdt_flush(uintptr gdt_ptr_addr);
extern void tss_flush(uint16 tss_selector);

static void gdt_write_tss_descriptor(uint64 base, uint32 limit) {
    uint64 low = 0;
    uint64 high = 0;

    low |= (uint64)(limit & 0xFFFFu);
    low |= (base & 0xFFFFFFull) << 16;
    low |= ((uint64)0x89u) << 40; /* present, DPL=0, available 64-bit TSS */
    low |= ((uint64)((limit >> 16) & 0x0Fu)) << 48;
    low |= ((base >> 24) & 0xFFull) << 56;

    high |= (base >> 32) & 0xFFFFFFFFull;

    gdt_entries[GDT_INDEX_TSS_LO] = low;
    gdt_entries[GDT_INDEX_TSS_HI] = high;
}

static void gdt_write_tss(uintptr kernel_rsp0) {
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.rsp0 = (uint64)kernel_rsp0;
    g_tss.iomap_base = (uint16)sizeof(g_tss);

    gdt_write_tss_descriptor((uint64)(uintptr)&g_tss, (uint32)(sizeof(g_tss) - 1u));
}

void tss_set_kernel_stack(uint32 esp0) {
    g_tss.rsp0 = (uint64)(uintptr)esp0;
}

void gdt_init(void) {
    uintptr rsp = 0;

    gdt_entries[0] = 0x0000000000000000ull;
    gdt_entries[1] = 0x00AF9A000000FFFFull; /* kernel 64-bit code */
    gdt_entries[2] = 0x00CF92000000FFFFull; /* kernel data */
    gdt_entries[3] = 0x00CFFA000000FFFFull; /* user 32-bit compat code */
    gdt_entries[4] = 0x00CFF2000000FFFFull; /* user data */
    gdt_entries[7] = 0x0000000000000000ull;

    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    gdt_write_tss(rsp);

    gdt_ptr.limit = (uint16)(sizeof(gdt_entries) - 1u);
    gdt_ptr.base = (uint64)(uintptr)&gdt_entries[0];

    gdt_flush((uintptr)&gdt_ptr);
    tss_flush(GDT_TSS_SEL);
}

void gdt_set_ldt_descriptor(uint32 base, uint32 limit) {
    (void)base;
    (void)limit;
}