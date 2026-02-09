#include <segdom.h>
#include <gdt.h>
#include <string.h>
#include <misc/types.h>

#define SEGDESC_ACC_CODE 0xFA
#define SEGDESC_ACC_DATA 0xF2
#define SEGDESC_GRAN_4K  0xCF
#define SEGDESC_GRAN_BYTE 0x40

static seg_desc_t segdesc_make(uint32 base, uint32 limit, uint8 access, uint8 gran) {
    seg_desc_t d;
    d.limit_low = (uint16)(limit & 0xFFFFu);
    d.base_low = (uint16)(base & 0xFFFFu);
    d.base_mid = (uint8)((base >> 16) & 0xFFu);
    d.access = access;
    d.gran = (uint8)((limit >> 16) & 0x0Fu);
    d.gran |= (gran & 0xF0u);
    d.base_high = (uint8)((base >> 24) & 0xFFu);
    return d;
}

static void segdom_build_ldt(segdom_t* dom, uint32 base, uint32 limit_bytes) {
    uint32 limit = limit_bytes;
    uint8 gran = SEGDESC_GRAN_BYTE;
    if (limit_bytes > 0xFFFFFu) {
        limit = (limit_bytes >> 12);
        gran = SEGDESC_GRAN_4K;
    }

    dom->ldt[0] = segdesc_make(0, 0, 0, 0);
    dom->ldt[SEGDOM_CS_INDEX] = segdesc_make(base, limit, SEGDESC_ACC_CODE, gran);
    dom->ldt[SEGDOM_DS_INDEX] = segdesc_make(base, limit, SEGDESC_ACC_DATA, gran);
}

void segdom_init(segdom_t* dom, uint32 base, uint32 limit_bytes) {
    if (!dom) return;
    memset(dom, 0, sizeof(*dom));

    dom->base = base;
    dom->limit_bytes = limit_bytes;
    dom->ldt_selector = GDT_LDT_SEL;
    dom->user_cs = (uint16)((SEGDOM_CS_INDEX << 3) | SEGDOM_SEL_TI | SEGDOM_RPL3);
    dom->user_ds = (uint16)((SEGDOM_DS_INDEX << 3) | SEGDOM_SEL_TI | SEGDOM_RPL3);

    segdom_build_ldt(dom, base, limit_bytes);
    gdt_set_ldt_descriptor((uint32)&dom->ldt[0], (uint32)(sizeof(dom->ldt) - 1));
}

void segdom_load(const segdom_t* dom) {
    if (!dom) return;
    uint16 sel = dom->ldt_selector;
    __asm__ __volatile__("lldt %0" : : "r"(sel));
}

int segdom_user_range_ok(const segdom_t* dom, uint32 addr, uint32 len) {
    if (!dom) return 0;
    if (len == 0) return 1;

    uint32 start = addr;
    uint32 end = addr + len;
    if (end < start) return 0;

    uint32 base = dom->base;
    uint32 limit = dom->limit_bytes;

    if (start < base) return 0;
    if (end > base + limit) return 0;

    return 1;
}
