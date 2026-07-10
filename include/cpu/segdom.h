#ifndef SEGDOM_H
#define SEGDOM_H

#include <misc/types.h>

#define SEGDOM_LDT_ENTRIES 3

// LDT selector bits
#define SEGDOM_SEL_TI 0x04
#define SEGDOM_RPL3 0x03
#define SEGDOM_CS_INDEX 1
#define SEGDOM_DS_INDEX 2

typedef struct seg_desc_t {
    uint16 limit_low;
    uint16 base_low;
    uint8 base_mid;
    uint8 access;
    uint8 gran;
    uint8 base_high;
} __attribute__((packed)) seg_desc_t;

typedef struct segdom_t {
    seg_desc_t ldt[SEGDOM_LDT_ENTRIES];
    uint16 ldt_selector;
    uint16 user_cs;
    uint16 user_ds;
    uint32 base;
    uint32 limit_bytes;
} segdom_t;

void segdom_init(segdom_t* dom, uint32 base, uint32 limit_bytes);
void segdom_load(const segdom_t* dom);
int segdom_user_range_ok(const segdom_t* dom, uint32 addr, uint32 len);

extern volatile uint16 g_user_segdom_cs;
extern volatile uint16 g_user_segdom_ds;
extern volatile uint16 g_user_segdom_gs;

#endif
