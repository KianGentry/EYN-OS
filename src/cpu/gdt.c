#include <gdt.h>
#include <string.h>

/* GDT entry structure */
typedef struct {
    uint16 limit_low;
    uint16 base_low;
    uint8  base_middle;
    uint8  access;
    uint8  granularity;
    uint8  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16 limit;
    uint32 base;
} __attribute__((packed)) gdt_ptr_t;

/* 32-bit TSS */
typedef struct {
    uint32 prev_tss;
    uint32 esp0;
    uint32 ss0;
    uint32 esp1;
    uint32 ss1;
    uint32 esp2;
    uint32 ss2;
    uint32 cr3;
    uint32 eip;
    uint32 eflags;
    uint32 eax;
    uint32 ecx;
    uint32 edx;
    uint32 ebx;
    uint32 esp;
    uint32 ebp;
    uint32 esi;
    uint32 edi;
    uint32 es;
    uint32 cs;
    uint32 ss;
    uint32 ds;
    uint32 fs;
    uint32 gs;
    uint32 ldt;
    uint16 trap;
    uint16 iomap_base;
} __attribute__((packed)) tss_entry_t;

static gdt_entry_t gdt_entries[6];
static gdt_ptr_t gdt_ptr;
static tss_entry_t g_tss;

extern void gdt_flush(uint32 gdt_ptr_addr);
extern void tss_flush(uint16 tss_selector);

static void gdt_set_gate(int num, uint32 base, uint32 limit, uint8 access, uint8 gran) {
    gdt_entries[num].base_low = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access = access;
}

static void write_tss(int num, uint16 ss0, uint32 esp0) {
    memset(&g_tss, 0, sizeof(g_tss));

    g_tss.ss0 = ss0;
    g_tss.esp0 = esp0;

    /* Ring-3 default segments when CPU does hardware task switching (not used yet)
       but good practice to initialize. */
    g_tss.cs = GDT_USER_CS;
    g_tss.ss = GDT_USER_DS;
    g_tss.ds = GDT_USER_DS;
    g_tss.es = GDT_USER_DS;
    g_tss.fs = GDT_USER_DS;
    g_tss.gs = GDT_USER_DS;

    g_tss.iomap_base = sizeof(g_tss);

    uint32 base = (uint32)&g_tss;
    uint32 limit = base + sizeof(g_tss);

    /* 0x89 = present, DPL=0, type=9 (32-bit available TSS) */
    gdt_set_gate(num, base, limit, 0x89, 0x00);
}

void tss_set_kernel_stack(uint32 esp0) {
    g_tss.esp0 = esp0;
}

void gdt_init(void) {
    gdt_ptr.limit = (uint16)(sizeof(gdt_entry_t) * 6 - 1);
    gdt_ptr.base = (uint32)&gdt_entries;

    /* 0: null */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* 1: kernel code (0x9A), 2: kernel data (0x92) */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* 3: user code (0xFA), 4: user data (0xF2) */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* 5: TSS */
    uint32 esp;
    asm volatile("mov %%esp, %0" : "=r"(esp));
    write_tss(5, GDT_KERNEL_DS, esp);

    gdt_flush((uint32)&gdt_ptr);
    tss_flush(GDT_TSS_SEL);
}
