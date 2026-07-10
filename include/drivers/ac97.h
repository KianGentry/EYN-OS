/*
 * AC97 audio controller driver for EYN-OS.
 *
 * Targets the Intel ICH-series AC'97 audio controller, which QEMU emulates
 * via `-audiodev` + `-device AC97` (or the default `-soundhw ac97`).
 *
 * Hardware overview
 * =================
 *   The AC97 audio subsystem consists of two components:
 *     1. An AC97 *codec* (the analog part -- DAC/ADC/mixer).
 *     2. An *AC97 controller* on the PCI bus (Intel ICH or compatible).
 *
 *   The controller exposes two I/O BAR regions:
 *     - BAR0 (NAM -- Native Audio Mixer): codec register access (16-bit I/O).
 *     - BAR1 (NABM -- Native Audio Bus-Mastering): DMA control (32-bit I/O).
 *
 *   Playback uses a Bus-Master Output (PCM-Out) channel.  The driver
 *   programs a Buffer Descriptor List (BDL) -- an array of up to 32 entries,
 *   each pointing to a PCM buffer + sample count.  The hardware walks the
 *   list, fetching and playing buffers via DMA, and raises an IRQ on each
 *   buffer completion (IOC) or list completion (LVBCI).
 *
 * Driver design (EYN-OS constraints)
 * ===================================
 *   - Single-stream, PCM-Out only (no recording).
 *   - Fixed 16-bit signed LE stereo at the AC97 codec's native 48 kHz
 *     (the most universally supported rate).  Higher-level code resamples
 *     if the source rate differs.
 *   - Double-buffered BDL: two 4 KB DMA buffers.  The IRQ handler flips
 *     between them; userland refills the idle half via a syscall.
 *   - Lazy init: the PCI scan + DMA rings are allocated on first audio
 *     syscall, not at kernel boot (saves RAM when audio is unused).
 *
 * ABI-INVARIANT: The AC97 output format is always 16-bit signed LE stereo
 *                at 48 000 Hz.  Callers must resample/convert before
 *                submitting buffers.
 *
 * SECURITY-INVARIANT: All DMA buffers are kernel-owned; userland data is
 *                     copied in via copyout/copyin.  User pointers are
 *                     validated before every access.
 */
#ifndef AC97_H
#define AC97_H

#include <misc/types.h>
#include <stdint.h>
#include <stddef.h>

/* ---- PCI identification ---- */

/*
 * ABI-INVARIANT: Intel ICH AC97 Audio Controller PCI IDs.
 *
 * Vendor 0x8086 (Intel), device 0x2415 (82801AA AC97 Audio).
 * QEMU's default AC97 emulation uses this ID.
 */
#define AC97_PCI_VENDOR  0x8086u
#define AC97_PCI_DEVICE  0x2415u

/* ---- AC97 codec (NAM) registers (offsets from BAR0) ---- */

#define AC97_NAM_RESET          0x00u  /* Reset codec                      */
#define AC97_NAM_MASTER_VOL     0x02u  /* Master volume (L/R, 6-bit atten) */
#define AC97_NAM_PCM_VOL        0x18u  /* PCM-out volume                   */
#define AC97_NAM_EXT_AUDIO_ID   0x28u  /* Extended audio ID                */
#define AC97_NAM_EXT_AUDIO_CTRL 0x2Au  /* Extended audio status/ctrl       */
#define AC97_NAM_PCM_FRONT_RATE 0x2Cu  /* Front DAC sample rate            */

/* ---- Bus-Master (NABM) registers (offsets from BAR1) ---- */

/*
 * Each DMA channel has a block of 0x20 bytes; PCM-Out is at offset 0x10.
 * Within each channel block:
 *   +0x00  BDBAR -- Buffer Descriptor list Base Address Register (32-bit)
 *   +0x04  CIV   -- Current Index Value (8-bit, read-only)
 *   +0x05  LVI   -- Last Valid Index     (8-bit, read/write)
 *   +0x06  SR    -- Status Register      (16-bit, W1C)
 *   +0x08  PICB  -- Position In Current Buffer (16-bit, read-only)
 *   +0x0A  PIV   -- Prefetched Index Value (8-bit, read-only)
 *   +0x0B  CR    -- Control Register     (8-bit, read/write)
 */
#define AC97_NABM_PCMOUT      0x10u  /* PCM-Out channel base              */
#define AC97_NABM_GLB_CTRL    0x2Cu  /* Global control                    */
#define AC97_NABM_GLB_STAT    0x30u  /* Global status                     */

/* Channel-relative offsets */
#define AC97_CH_BDBAR  0x00u
#define AC97_CH_CIV    0x04u
#define AC97_CH_LVI    0x05u
#define AC97_CH_SR     0x06u
#define AC97_CH_PICB   0x08u
#define AC97_CH_PIV    0x0Au
#define AC97_CH_CR     0x0Bu

/* Control Register bits */
#define AC97_CR_RPBM   (1u << 0)  /* Run / Pause Bus Master              */
#define AC97_CR_RR     (1u << 1)  /* Reset Registers                     */
#define AC97_CR_LVBIE  (1u << 2)  /* Last Valid Buffer Interrupt Enable   */
#define AC97_CR_FEIE   (1u << 3)  /* FIFO Error Interrupt Enable         */
#define AC97_CR_IOCE   (1u << 4)  /* Interrupt On Completion Enable      */

/* Status Register bits (W1C) */
#define AC97_SR_DCH    (1u << 0)  /* DMA Controller Halted               */
#define AC97_SR_CELV   (1u << 1)  /* Current Equals Last Valid           */
#define AC97_SR_LVBCI  (1u << 2)  /* Last Valid Buffer Completion Int    */
#define AC97_SR_BCIS   (1u << 3)  /* Buffer Completion Interrupt Status  */
#define AC97_SR_FIFOE  (1u << 4)  /* FIFO Error                         */

/* Global Control bits */
#define AC97_GC_GIE    (1u << 0)  /* Global Interrupt Enable             */
#define AC97_GC_CR     (1u << 1)  /* Cold Reset                         */

/*
 * SECURITY-INVARIANT: Buffer Descriptor List entry count.
 *
 * The AC97 hardware supports up to 32 BDL entries.  We use all 32 in a
 * circular ring.  write_idx cycles modulo AC97_BDL_COUNT so LVI always
 * advances forward -- never backwards -- preventing hardware stalls caused
 * by CIV > LVI.
 *
 * Breakage if changed: must remain ≤ 32; write_idx wraps at this value.
 */
#define AC97_BDL_COUNT 32u

/*
 * ABI-INVARIANT: Number of physical DMA buffers.
 *
 * We allocate one unique 4 KB PCM buffer per BDL slot.  Earlier revisions
 * reused 8 physical buffers across 32 descriptors, but the AC97 controller
 * can prefetch ahead of CIV, so reusing a buffer too early caused audible
 * short-loop repetition as future audio overwrote data the controller had not
 * fully consumed yet.
 *
 * We intentionally keep one logical ring slot empty when queueing, so the
 * maximum in-flight descriptor count is AC97_DMA_COUNT - 1.  This avoids the
 * CIV==LVI ambiguity where the hardware state cannot distinguish "1 slot in
 * flight" from "all 32 slots in flight" using only CIV/LVI/DCH polling.
 */
#define AC97_DMA_COUNT AC97_BDL_COUNT

/*
 * SECURITY-INVARIANT: Per-buffer DMA size in bytes.
 *
 * 4096 bytes = 1024 stereo 16-bit samples = ~21.3 ms at 48 kHz.
 * The buffer must be physically contiguous and below 4 GB.
 *
 * Breakage if changed: BDL sample-count fields must be updated in tandem.
 */
#define AC97_DMA_BUF_SIZE 4096u

/*
 * Number of 16-bit samples per DMA buffer.
 * AC97 BDL counts in 16-bit samples (not bytes, not frames).
 */
#define AC97_DMA_SAMPLES (AC97_DMA_BUF_SIZE / 2u)

/* ---- Buffer Descriptor List entry (8 bytes, naturally aligned) ---- */
typedef struct __attribute__((packed)) {
    uint32_t addr;    /* physical address of PCM buffer                  */
    uint16_t samples; /* number of 16-bit samples in buffer              */
    uint16_t flags;   /* bit 15 = IOC (interrupt on completion),
                         bit 14 = BUP (buffer underrun policy)           */
} ac97_bdl_entry_t;

#define AC97_BDL_FLAG_IOC (1u << 15)
#define AC97_BDL_FLAG_BUP (1u << 14)

/* ---- Driver state ---- */

typedef struct {
    int      found;          /* 1 if PCI device was found                */
    int      initialised;    /* 1 if DMA rings are set up                */
    int      playing;        /* 1 if DMA is running                      */

    /* PCI location */
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;
    uint8_t  irq_line;

    /* I/O base addresses (from PCI BARs) */
    uint16_t nam_base;       /* BAR0: Native Audio Mixer                 */
    uint16_t nabm_base;      /* BAR1: Native Audio Bus-Master            */

    /* DMA bookkeeping */
    ac97_bdl_entry_t* bdl;                   /* 32-entry BDL array                        */
    uint8_t*          dma_buf[AC97_DMA_COUNT]; /* one physical PCM buffer per BDL slot      */
    /*
     * write_idx: monotonically increasing counter; the actual BDL slot index
     * is (write_idx % AC97_BDL_COUNT) and the physical buffer index is
     * equal to the BDL slot index.  Never decrements, so LVI always
     * advances forward and hardware never stalls from CIV > LVI.
     */
    uint32_t          write_idx;
} ac97_state_t;

/* ---- Public API ---- */

/*
 * Probe for an AC97 audio controller on PCI.
 * Returns 0 if found (populates internal state), -1 if absent.
 * Does NOT allocate DMA buffers or enable interrupts.
 */
int ac97_probe(void);

/*
 * Initialise the AC97 controller: allocate DMA buffers, program the codec,
 * register the IRQ handler, and prepare for playback.
 * Must be called after ac97_probe() returns 0.
 * Returns 0 on success, -1 on failure.
 */
int ac97_init(void);

/*
 * Submit a buffer of PCM data for playback.
 *
 * `data` must point to 16-bit signed LE stereo PCM at 48 kHz.
 * `size` is the byte length (must be ≤ AC97_DMA_BUF_SIZE).
 *
 * Non-blocking.  Returns 0 on success, -1 if the ring is full (caller
 * should stop submitting for this frame) or on invalid arguments.
 * Ring-full is detected by polling the hardware CIV/DCH registers, not by
 * interrupt-driven counters, so it is accurate regardless of IRQ latency.
 * The implementation keeps one ring slot empty, so at most 31 descriptors are
 * in flight at once.
 * Only restarts DMA when the engine is actually halted (DCH=1), avoiding
 * gratuitous SR/CR writes that can cause audible glitches on QEMU.
 */
int ac97_write(const void* data, size_t size);

/*
 * Submit a large buffer of PCM data, filling as many ring slots as possible
 * in one call.  The buffer is split into AC97_DMA_BUF_SIZE (4 KB) chunks;
 * the final chunk is zero-padded if shorter.
 *
 * `data` must point to 16-bit signed LE stereo PCM at 48 kHz.
 * `total_size` is the total byte length of the buffer.
 *
 * Non-blocking.  Returns the number of 4 KB ring slots filled (>= 0),
 * or -1 on invalid arguments.  0 means the ring was already full.
 * DMA is restarted at most once, after all slots have been queued.
 */
int ac97_write_bulk(const void* data, size_t total_size);

/*
 * Stop playback and silence the output.
 */
void ac97_stop(void);

/*
 * Resume playback after ac97_stop().
 */
void ac97_resume(void);

/*
 * Query whether the AC97 controller was detected and initialised.
 * Returns 1 if audio is available, 0 otherwise.
 */
int ac97_is_available(void);

/*
 * Return a read-only pointer to the driver state (for diagnostics).
 */
const ac97_state_t* ac97_get_state(void);

#endif /* AC97_H */
