/*
 * AC97 audio controller driver -- Intel ICH-compatible.
 *
 * See include/drivers/ac97.h for hardware overview and design rationale.
 *
 * Memory ownership
 * ================
 *   All DMA buffers and the BDL are allocated via kmalloc() (slab allocator)
 *   on first ac97_init().  They are never freed (the audio subsystem is
 *   expected to live for the kernel's lifetime once activated).
 *
 * Locking
 * =======
 *   Single-threaded by design.  The ring-full check in ac97_write reads
 *   hardware registers (CIV, SR.DCH) directly instead of relying on an
 *   IRQ-maintained counter.  This is necessary because BCIS is a single
 *   sticky bit: if N buffers complete before the IRQ is serviced, BCIS
 *   fires once and a counter-decrement approach would only credit one
 *   completion, permanently understating available slots after the initial
 *   fill, freezing playback at ~170 ms.
 *
 *   The IRQ handler still runs to clear BCIS/LVBCI and send EOI, but it
 *   no longer drives any shared state.
 *
 * Failure semantics
 * =================
 *   If PCI scan finds no device, ac97_probe() returns -1 and all subsequent
 *   calls are no-ops.  If DMA allocation fails, ac97_init() returns -1.
 */

#include <drivers/ac97.h>
#include <drivers/system.h>
#include <drivers/serial.h>
#include <drivers/vga.h>
#include <cpu/irq.h>
#include <mm/vmm.h>
#include <misc/types.h>
#include <util.h>
#include <stdint.h>
#include <stddef.h>

/* ---- Internal helpers for string operations (freestanding) ---- */
static void ac97_memset(void* dst, uint8_t val, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; ++i) d[i] = val;
}

static void ac97_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}

/*
 * ---- Private PCI configuration-space accessors ----
 *
 * The shared pci_read_config_dword() API is gated behind a capability check
 * (CAP_DEV_NET) via the current_command_context.  When ac97_probe() is called
 * during a user syscall, the context carries user-process capabilities, which
 * do not include CAP_DEV_NET, so every read returns 0xFFFFFFFF -- making the
 * entire PCI bus appear empty.
 *
 * As a kernel driver, the AC97 code is entitled to access config space
 * directly.  These statics bypass the capability layer and write/read the
 * standard PCI mechanism-1 ports (0xCF8 / 0xCFC) themselves.
 *
 * ABI-INVARIANT: Uses PCI configuration mechanism #1 (universal on x86 since
 * PCI 2.0).  Address format: enable(31) | bus(23:16) | dev(15:11) |
 * fn(10:8) | reg(7:2) | 00.
 */
#define AC97_PCI_ADDR_PORT 0xCF8u
#define AC97_PCI_DATA_PORT 0xCFCu

static uint32_t ac97_pci_make_addr(uint8_t bus, uint8_t dev,
                                   uint8_t func, uint8_t offset) {
    return 0x80000000u
         | ((uint32_t)bus    << 16)
         | ((uint32_t)dev    << 11)
         | ((uint32_t)func   <<  8)
         | ((uint32_t)(offset & 0xFCu));
}

static uint32_t ac97_pci_read32(uint8_t bus, uint8_t dev,
                                uint8_t func, uint8_t offset) {
    outl(AC97_PCI_ADDR_PORT, ac97_pci_make_addr(bus, dev, func, offset));
    return inl(AC97_PCI_DATA_PORT);
}

static uint16_t ac97_pci_read16(uint8_t bus, uint8_t dev,
                                uint8_t func, uint8_t offset) {
    uint32_t dword = ac97_pci_read32(bus, dev, func, offset & 0xFCu);
    return (uint16_t)(dword >> ((offset & 2u) * 8u));
}

static uint8_t ac97_pci_read8(uint8_t bus, uint8_t dev,
                              uint8_t func, uint8_t offset) {
    uint32_t dword = ac97_pci_read32(bus, dev, func, offset & 0xFCu);
    return (uint8_t)(dword >> ((offset & 3u) * 8u));
}

static void ac97_pci_write32(uint8_t bus, uint8_t dev,
                             uint8_t func, uint8_t offset, uint32_t val) {
    outl(AC97_PCI_ADDR_PORT, ac97_pci_make_addr(bus, dev, func, offset));
    outl(AC97_PCI_DATA_PORT, val);
}

/* ---- Global driver state ---- */
static ac97_state_t g_ac97;

/* ---- NAM (codec) access ---- */

static uint16_t nam_read16(uint16_t reg) {
    return inw(g_ac97.nam_base + reg);
}

static void nam_write16(uint16_t reg, uint16_t val) {
    outw(g_ac97.nam_base + reg, val);
}

/* ---- NABM (bus-master) access ---- */

static uint16_t nabm_read16(uint16_t off) {
    return inw(g_ac97.nabm_base + off);
}

static uint8_t nabm_read8(uint16_t off) {
    return inportb(g_ac97.nabm_base + off);
}

static void nabm_write8(uint16_t off, uint8_t val) {
    outportb(g_ac97.nabm_base + off, val);
}

static void nabm_write16(uint16_t off, uint16_t val) {
    outw(g_ac97.nabm_base + off, val);
}

static void nabm_write32(uint16_t off, uint32_t val) {
    outl(g_ac97.nabm_base + off, val);
}

static int ac97_hw_in_flight(void);

static uint32_t ac97_ptr_to_u32(const void* ptr) {
    uintptr raw = (uintptr)ptr;
    uint32_t narrowed = (uint32_t)raw;
    if ((uintptr)narrowed != raw) {
        return 0;
    }
    return narrowed;
}

static uint32_t ac97_dma_phys(const void* ptr) {
    return vmm_virt_to_phys(&vmm_kernel_as, ac97_ptr_to_u32(ptr));
}

static void ac97_debug_dump_runtime(const char* tag, int slot) {
    static int debug_budget = 12;
    uint16_t sr;
    uint8_t civ;
    uint8_t lvi;
    uint16_t picb;

    if (debug_budget <= 0) return;
    debug_budget--;

    sr = nabm_read16(AC97_NABM_PCMOUT + AC97_CH_SR);
    civ = nabm_read8(AC97_NABM_PCMOUT + AC97_CH_CIV) & 0x1Fu;
    lvi = nabm_read8(AC97_NABM_PCMOUT + AC97_CH_LVI) & 0x1Fu;
    picb = nabm_read16(AC97_NABM_PCMOUT + AC97_CH_PICB);

    printf("[AC97] %s slot=%d sr=0x%X civ=%u lvi=%u picb=%u hw=%d play=%d\n",
           tag,
           slot,
           (unsigned)sr,
           (unsigned)civ,
           (unsigned)lvi,
           (unsigned)picb,
           ac97_hw_in_flight(),
           g_ac97.playing);
}

/* ---- Ring-full probe (polled) ---- */

/*
 * Compute the number of BDL entries currently queued or being played,
 * by reading hardware registers rather than counting IRQ firings.
 *
 * Why polled, not IRQ-driven:
 *   BCIS is a single sticky W1C bit.  If N buffers complete before the
 *   IRQ is serviced (e.g. while IF=0 inside a syscall), BCIS is set once.
 *   A counter that decrements by 1 per BCIS-IRQ would undercount by N-1,
 *   eventually stalling the ring-full check permanently.
 *
 * Hardware registers used:
 *   SR.DCH (bit 0): DMA halted.  When set, no entry is in flight.
 *   CIV     (1 byte at CH+0x04): Current Index Value -- slot being played.
 *   LVI     (1 byte at CH+0x05): Last Valid Index    -- last slot submitted.
 *
 * When running: slots CIV..LVI (mod 32, inclusive) are in flight.
 *   count = (LVI - CIV + 32) % 32 + 1.
 * When halted (DCH=1): 0 slots in flight.
 */
static int ac97_hw_in_flight(void) {
    uint16_t sr = nabm_read16(AC97_NABM_PCMOUT + AC97_CH_SR);
    if (sr & AC97_SR_DCH) return 0;
    uint8_t civ = nabm_read8(AC97_NABM_PCMOUT + AC97_CH_CIV) & 0x1Fu;
    uint8_t lvi = nabm_read8(AC97_NABM_PCMOUT + AC97_CH_LVI) & 0x1Fu;
    return (int)((uint8_t)((lvi - civ + 32u) % 32u) + 1u);
}

/* ---- IRQ handler ---- */

static void ac97_irq_handler(void) {
    /*
     * ACK all sources (W1C).  The write side no longer uses BCIS to count
     * completions; it polls CIV/DCH directly.  The handler exists to clear
     * the sticky bits so the PIC is not held asserted and to send EOI.
     */
    nabm_write16(AC97_NABM_PCMOUT + AC97_CH_SR,
                 AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE);
    pic_send_eoi(g_ac97.irq_line);
}

/* ---- PCI setup helpers ---- */

/*
 * Walk the PCI bus looking for vendor/device = AC97_PCI_VENDOR/AC97_PCI_DEVICE.
 */
static int ac97_find_pci(void) {
    for (int bus = 0; bus < 256; ++bus) {
        for (int dev = 0; dev < 32; ++dev) {
            for (int func = 0; func < 8; ++func) {
                uint32_t reg0 = ac97_pci_read32((uint8_t)bus, (uint8_t)dev,
                                                (uint8_t)func, 0x00);
                if (reg0 == 0xFFFFFFFFu) continue;

                uint16_t vendor = (uint16_t)(reg0 & 0xFFFFu);
                uint16_t device = (uint16_t)(reg0 >> 16);

                if (vendor == AC97_PCI_VENDOR && device == AC97_PCI_DEVICE) {
                    g_ac97.pci_bus  = (uint8_t)bus;
                    g_ac97.pci_dev  = (uint8_t)dev;
                    g_ac97.pci_func = (uint8_t)func;

                    /* Read BAR0 (NAM) and BAR1 (NABM) -- I/O space */
                    uint32_t bar0 = ac97_pci_read32((uint8_t)bus, (uint8_t)dev,
                                                    (uint8_t)func, 0x10);
                    uint32_t bar1 = ac97_pci_read32((uint8_t)bus, (uint8_t)dev,
                                                    (uint8_t)func, 0x14);

                    g_ac97.nam_base  = (uint16_t)(bar0 & 0xFFFCu);
                    g_ac97.nabm_base = (uint16_t)(bar1 & 0xFFFCu);

                    /* Read IRQ line */
                    g_ac97.irq_line = ac97_pci_read8((uint8_t)bus, (uint8_t)dev,
                                                     (uint8_t)func, 0x3C);

                    /* Enable I/O space (bit 0) + bus-mastering (bit 2). */
                    uint16_t cmd = ac97_pci_read16((uint8_t)bus, (uint8_t)dev,
                                                   (uint8_t)func, 0x04);
                    cmd |= 0x05u;
                    ac97_pci_write32((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                                    0x04, (uint32_t)cmd);

                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ---- Public API ---- */

int ac97_probe(void) {
    if (g_ac97.found) return 0;

    ac97_memset(&g_ac97, 0, sizeof(g_ac97));
    if (ac97_find_pci() != 0) {
        serial_write(SERIAL_COM1, "[AC97] No AC97 controller found on PCI bus\n", 43);
        return -1;
    }

    g_ac97.found = 1;
    serial_write(SERIAL_COM1, "[AC97] Found AC97 controller on PCI\n", 37);
    return 0;
}

int ac97_init(void) {
    if (!g_ac97.found) {
        if (ac97_probe() != 0) return -1;
    }
    if (g_ac97.initialised) return 0;

    /* ---- 1. Cold reset the controller ---- */
    nabm_write32(AC97_NABM_GLB_CTRL, AC97_GC_CR);

    /* Brief delay for reset to settle (spin ~100k iterations ≈ a few ms). */
    for (volatile int i = 0; i < 100000; ++i) { (void)i; }

    nabm_write32(AC97_NABM_GLB_CTRL, AC97_GC_GIE);

    /* ---- 2. Reset the codec ---- */
    nam_write16(AC97_NAM_RESET, 0x0000u);
    for (volatile int i = 0; i < 100000; ++i) { (void)i; }

    /* ---- 3. Set volumes ---- */
    nam_write16(AC97_NAM_MASTER_VOL, 0x0000u);  /* full volume */
    nam_write16(AC97_NAM_PCM_VOL,    0x0000u);  /* full PCM volume */
        printf("[AC97] mixer master=0x%X pcm=0x%X\n",
            (unsigned)nam_read16(AC97_NAM_MASTER_VOL),
            (unsigned)nam_read16(AC97_NAM_PCM_VOL));

    /* ---- 4. Check for Variable Rate Audio support ---- */
    uint16_t ext_id = nam_read16(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001u) {
        /* VRA supported -- enable it and set 48 kHz. */
        uint16_t ext_ctrl = nam_read16(AC97_NAM_EXT_AUDIO_CTRL);
        ext_ctrl |= 0x0001u;  /* enable VRA */
        nam_write16(AC97_NAM_EXT_AUDIO_CTRL, ext_ctrl);
        nam_write16(AC97_NAM_PCM_FRONT_RATE, 48000u);
    }
    /* If VRA is not supported, the codec runs at its fixed 48 kHz anyway. */

    /* ---- 5. Allocate DMA buffers ---- */
    /*
     * We need:
     *   - 32-entry BDL (32 × 8 = 256 bytes, physically contiguous)
    *   - AC97_DMA_COUNT × 4096-byte PCM buffers (32 × 4096 = 128 KB)
     *
     * All allocations go through kmalloc.  On EYN-OS without full
     * VA-to-PA translation, kernel VA == PA for low memory.
     *
    * Having one physical buffer per BDL slot avoids reusing backing memory
    * while the controller may still have prefetched that descriptor.  We
    * keep one logical slot empty, so up to 31 slots may be in flight,
    * giving ~660 ms of pre-buffered audio at 48 kHz.
     */
    g_ac97.bdl = (ac97_bdl_entry_t*)vmm_kmalloc_aligned((uint32)(AC97_BDL_COUNT * sizeof(ac97_bdl_entry_t)));
    if (!g_ac97.bdl) {
        serial_write(SERIAL_COM1, "[AC97] Failed to allocate BDL\n", 31);
        return -1;
    }
    ac97_memset(g_ac97.bdl, 0, PAGE_SIZE);

    for (int i = 0; i < (int)AC97_DMA_COUNT; ++i) {
        g_ac97.dma_buf[i] = (uint8_t*)vmm_kmalloc_aligned(AC97_DMA_BUF_SIZE);
        if (!g_ac97.dma_buf[i]) {
            serial_write(SERIAL_COM1, "[AC97] Failed to allocate DMA buffer\n", 38);
            return -1;
        }
        ac97_memset(g_ac97.dma_buf[i], 0, AC97_DMA_BUF_SIZE);
    }

    /* ---- 6. Program BDL ---- */
    /*
     * Pre-wire all 32 BDL entries one-to-one with physical PCM buffers.
     * Descriptor i always points at dma_buf[i].  This avoids the audible
     * repetition caused by recycling a smaller backing-buffer set while the
     * AC97 controller may still have descriptors prefetched ahead of CIV.
     */
    for (int i = 0; i < (int)AC97_BDL_COUNT; ++i) {
        g_ac97.bdl[i].addr    = ac97_dma_phys(g_ac97.dma_buf[i]);
        g_ac97.bdl[i].samples = AC97_DMA_SAMPLES;
        /*
         * No IOC flag -- we use purely polled ring management.  IOC would
         * fire ~47 IRQs/sec at 48 kHz, each causing a context switch that
         * adds latency without benefit (ac97_hw_in_flight polls CIV/DCH
         * directly).  The hardware still sets SR.DCH and SR.LVBCI when it
         * halts, which is all we need to detect and restart.
         */
        g_ac97.bdl[i].flags   = 0;
    }

    g_ac97.write_idx = 0;

    /* ---- 7. Stop PCM-Out channel before programming ---- */
    nabm_write8(AC97_NABM_PCMOUT + AC97_CH_CR, AC97_CR_RR);
    for (volatile int i = 0; i < 10000; ++i) { (void)i; }

    /* ---- 8. Set BDBAR ---- */
    nabm_write32(AC97_NABM_PCMOUT + AC97_CH_BDBAR,
                 ac97_dma_phys(g_ac97.bdl));

    /* ---- 9. Register IRQ handler and unmask in PIC ---- */
    if (g_ac97.irq_line < 16) {
        register_interrupt_handler((int)g_ac97.irq_line, ac97_irq_handler);

        /*
         * Explicitly unmask the AC97 IRQ line in the 8259 PIC.
         *
         * irq_init() only unmasks IRQ0 (PIT) by name; all other IRQ bits are
         * left in whatever state the BIOS left them.  If the BIOS masked this
         * line, every buffer-completion interrupt would be swallowed silently,
         * `in_flight` would never decrement, and playback would stall after the
         * initial queue fill.
         *
         * IRQs 0-7 are on PIC1 (port 0x21); IRQs 8-15 are on PIC2 (port 0xA1).
         * For PIC2 IRQs, IRQ2 (cascade line) on PIC1 must also be unmasked.
         */
        if (g_ac97.irq_line < 8) {
            uint8_t mask = inportb(0x21);
            mask &= (uint8_t)(~(1u << g_ac97.irq_line));
            outportb(0x21, mask);
        } else {
            /* Unmask cascaded IRQ on PIC2. */
            uint8_t mask2 = inportb(0xA1);
            mask2 &= (uint8_t)(~(1u << (g_ac97.irq_line - 8u)));
            outportb(0xA1, mask2);
            /* Ensure IRQ2 (cascade) is not masked on PIC1. */
            uint8_t mask1 = inportb(0x21);
            mask1 &= (uint8_t)(~(1u << 2));
            outportb(0x21, mask1);
        }
    }

    g_ac97.initialised = 1;
    g_ac97.playing     = 0;

    serial_write(SERIAL_COM1, "[AC97] Initialised\n", 19);
    printf("[AC97] Audio controller initialised\n");
    return 0;
}

/*
 * Queue one buffer into the next BDL slot without restarting DMA.
 * Returns 0 on success, -1 if ring is full or args are invalid.
 *
 * Callers must call ac97_ensure_running() after queueing one or more
 * buffers to restart the DMA engine if it halted.
 */
static int ac97_queue_buffer(const void* data, size_t size) {
    if (!g_ac97.initialised) return -1;
    if (!data || size == 0) return -1;
    if (size > AC97_DMA_BUF_SIZE) size = AC97_DMA_BUF_SIZE;

    if (ac97_hw_in_flight() >= (int)(AC97_BDL_COUNT - 1u)) return -1;

    int slot = (int)(g_ac97.write_idx % (uint32_t)AC97_BDL_COUNT);

    /* Copy PCM data into the physical buffer and zero-pad the remainder. */
    ac97_memcpy(g_ac97.dma_buf[slot], data, size);
    if (size < AC97_DMA_BUF_SIZE)
        ac97_memset(g_ac97.dma_buf[slot] + size, 0, AC97_DMA_BUF_SIZE - size);

    g_ac97.bdl[slot].samples = AC97_DMA_SAMPLES;

    g_ac97.write_idx++;
    nabm_write8(AC97_NABM_PCMOUT + AC97_CH_LVI, (uint8_t)slot);
    return 0;
}

/*
 * Restart the DMA engine if it halted (DCH=1).  When the controller
 * reaches LVI it sets LVBCI + DCH and stops.  We must clear LVBCI
 * before re-asserting RPBM, otherwise the hardware refuses to resume.
 *
 * When DMA is already running (DCH=0), this is a no-op -- avoids the
 * per-buffer SR/CR I/O writes that caused audible glitches on QEMU.
 */
static void ac97_ensure_running(void) {
    uint16_t sr = nabm_read16(AC97_NABM_PCMOUT + AC97_CH_SR);
    if (sr & AC97_SR_DCH) {
        /* Halted -- clear sticky status then start bus master. */
        nabm_write16(AC97_NABM_PCMOUT + AC97_CH_SR,
                     AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE);
        nabm_write8(AC97_NABM_PCMOUT + AC97_CH_CR, AC97_CR_RPBM);
    }
    g_ac97.playing = 1;
}

int ac97_write(const void* data, size_t size) {
    int rc = ac97_queue_buffer(data, size);
    if (rc == 0)
        ac97_ensure_running();
    return rc;
}

int ac97_write_bulk(const void* data, size_t total_size) {
    if (!g_ac97.initialised) return -1;
    if (!data || total_size == 0) return -1;

    const uint8_t* p = (const uint8_t*)data;
    size_t remaining = total_size;
    int count = 0;

    while (remaining > 0) {
        size_t chunk = remaining > AC97_DMA_BUF_SIZE
                     ? AC97_DMA_BUF_SIZE : remaining;
        if (ac97_queue_buffer(p, chunk) < 0)
            break;   /* ring full */
        p         += chunk;
        remaining -= chunk;
        count++;
    }

    if (count > 0)
        ac97_ensure_running();

    return count;
}

void ac97_stop(void) {
    if (!g_ac97.initialised) return;

    /* Halt the DMA engine. */
    nabm_write8(AC97_NABM_PCMOUT + AC97_CH_CR, 0);
    g_ac97.playing = 0;

    /* Silence all physical buffers. */
    for (int i = 0; i < (int)AC97_DMA_COUNT; ++i)
        ac97_memset(g_ac97.dma_buf[i], 0, AC97_DMA_BUF_SIZE);
}

void ac97_resume(void) {
    if (!g_ac97.initialised || g_ac97.playing) return;
    ac97_ensure_running();
}

int ac97_is_available(void) {
    return g_ac97.initialised ? 1 : 0;
}

const ac97_state_t* ac97_get_state(void) {
    return &g_ac97;
}
