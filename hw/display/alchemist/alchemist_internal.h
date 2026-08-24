/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - shared internal state
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_DISPLAY_ALCHEMIST_INTERNAL_H
#define HW_DISPLAY_ALCHEMIST_INTERNAL_H

#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"

/*
 * BAR0 (GTTMMADR): combined MMIO register space + Global GTT. The real xe
 * driver's xe_mmio_probe_early() rejects anything smaller than 16MB before
 * reading a single register, so this is a hard floor, not a tuning knob.
 */
#define ALCHEMIST_MMIO_SIZE (16 * MiB)

/*
 * BAR2 (GMADR): local-memory/VRAM aperture. Real DG2 cards expose several
 * GB here; we start intentionally small and revisit if a later phase's
 * VRAM-size probe rejects it.
 */
#define ALCHEMIST_VRAM_SIZE (256 * MiB)

/*
 * CTB (Command Transport Buffer) registration state - the GGTT addresses
 * and dword-sizes the guest tells us about via SELF_CFG (see
 * alchemist_guc.c), used by alchemist_ctb.c to actually walk the rings.
 * A *_ctb_size of 0 means "not yet registered".
 */
typedef struct AlchemistCtb {
    uint64_t desc_addr;
    uint64_t ring_addr;
    uint32_t ring_size_dwords;
} AlchemistCtb;

/*
 * Satellite GuC coprocessor process - see alchemist_guc_proc.c. pid/qmp_fd
 * are -1 whenever no process is running (both explicitly set that way by
 * alchemist_guc_proc_start(), not relied on as a QOM zero-init default -
 * fd 0 is a real, valid fd number).
 */
typedef struct AlchemistGucProc {
    pid_t pid;
    int qmp_fd;
    char *qmp_path;
} AlchemistGucProc;

/*
 * Per-context state registered via GUC_ACTION_REGISTER_CONTEXT (see
 * alchemist_submit.c) - just enough to find a guc_id's LRC when a later
 * SCHED_CONTEXT/SCHED_CONTEXT_MODE_SET message arrives. Sized for early
 * command-submission bring-up (real guc_id space is larger); an
 * out-of-range guc_id is bounds-checked and ignored, not undefined
 * behavior.
 */
#define ALCHEMIST_MAX_CONTEXTS 64

typedef struct AlchemistContext {
    bool registered;
    uint64_t lrc_ggtt_addr; /* PPHWSP GGTT address - xe_lrc_ggtt_addr() */
} AlchemistContext;

typedef struct AlchemistState {
    PCIDevice pdev;
    MemoryRegion mmio;
    MemoryRegion vram;
    uint8_t *mmio_buf;
    uint8_t *vram_ptr;
    AlchemistCtb h2g;
    AlchemistCtb g2h;
    AlchemistGucProc guc_proc;
    AlchemistContext ctx[ALCHEMIST_MAX_CONTEXTS];
} AlchemistState;

static inline uint32_t alchemist_mmio_load32(AlchemistState *s, hwaddr addr)
{
    uint32_t val;

    memcpy(&val, s->mmio_buf + addr, sizeof(val));
    return val;
}

static inline void alchemist_mmio_store32(AlchemistState *s, hwaddr addr,
                                           uint32_t val)
{
    memcpy(s->mmio_buf + addr, &val, sizeof(val));
}

/*
 * Phase-specific write hooks. Each is called after the generic buffer
 * store has already happened (so DATA/param registers written just
 * before a "go" register are already visible), and handles side effects
 * for its own register(s) only - everything else stays plain
 * read/write-what-was-written memory.
 */
void alchemist_pcode_mmio_write(AlchemistState *s, hwaddr addr, unsigned size);
void alchemist_forcewake_mmio_write(AlchemistState *s, hwaddr addr, unsigned size);
void alchemist_guc_mmio_write(AlchemistState *s, hwaddr addr, unsigned size);

/*
 * Called once from realize() to pre-populate registers that just report a
 * fixed value rather than reacting to guest writes.
 */
void alchemist_vram_init(AlchemistState *s);

/*
 * GGTT (Global GTT) translation - see alchemist_ggtt.c. Reads/writes real
 * guest memory (system RAM via PCI DMA, or our own VRAM buffer) through
 * whatever PTEs the guest has written into the GGTT region of BAR0.
 * Returns false (nothing is transferred, or a short transfer already done
 * is left in place) if any page in the range is unmapped or the DMA
 * itself fails - callers must check the return value, there is no hidden
 * fallback.
 */
bool alchemist_ggtt_read(AlchemistState *s, uint64_t ggtt_addr, void *buf,
                          uint64_t len);
bool alchemist_ggtt_write(AlchemistState *s, uint64_t ggtt_addr,
                           const void *buf, uint64_t len);

/*
 * GT interrupt cascade - see alchemist_irq.c. Raises the one interrupt
 * source we currently model (GuC2Host) through the real multi-level
 * status/identity register chain, then fires the actual MSI.
 *
 * DG1_MSTR_TILE_INTR/GFX_MSTR_IRQ/GT_INTR_DW are write-1-to-clear status
 * registers, not plain memory - alchemist_irq_is_status_reg() lets
 * alchemist_mmio_write() route writes to those three addresses through
 * alchemist_irq_status_write() *instead of* the generic buffer store
 * every other register uses (see the file comment in alchemist_irq.c).
 * IIR_REG_SELECTOR is a plain register and goes through the normal
 * generic-store-then-react path via alchemist_irq_mmio_write().
 */
bool alchemist_irq_is_status_reg(hwaddr addr);
void alchemist_irq_status_write(AlchemistState *s, hwaddr addr, uint64_t val);
void alchemist_irq_raise_guc2host(AlchemistState *s);
void alchemist_irq_raise_rcs0(AlchemistState *s);
void alchemist_irq_mmio_write(AlchemistState *s, hwaddr addr, unsigned size);

/*
 * CTB (Command Transport Buffer) - see alchemist_ctb.c. Called from the
 * GuC mmio mailbox's SELF_CFG handler (alchemist_guc.c) to record a
 * registered ring's GGTT address/size, and from the GUC_HOST_INTERRUPT
 * doorbell handler to check for and process new H2G ring traffic.
 */
void alchemist_ctb_register(AlchemistState *s, uint16_t key, uint64_t val);
void alchemist_ctb_check_h2g(AlchemistState *s);
void alchemist_ctb_send_sched_context_mode_done(AlchemistState *s,
                                                 uint32_t guc_id,
                                                 uint32_t runnable_state);

/*
 * Command submission - see alchemist_submit.c. Called from
 * alchemist_ctb_check_h2g() for HXG_TYPE_FAST_REQUEST/EVENT messages
 * (GUC_ACTION_REGISTER_CONTEXT, XE_GUC_ACTION_SCHED_CONTEXT[_MODE_SET]) -
 * these never get a synchronous CTB reply (see alchemist_ctb.c's file
 * comment), so this only ever has side effects, never a return value to
 * send back on this same call.
 */
void alchemist_submit_handle_action(AlchemistState *s, uint32_t action,
                                     const uint32_t *payload, uint32_t n);

/*
 * Satellite GuC coprocessor process - see alchemist_guc_proc.c. Launches a
 * second qemu-system-x86_64 (-machine none -accel tcg -cpu 486, started
 * halted with -S) that will eventually run GuC's real firmware, and
 * QMP-controls it. Started once from realize(), stopped from exit() - the
 * process exists for the PCI device's whole lifetime, the same way the
 * real GuC die is powered whenever the card is, even though its boot ROM
 * doesn't actually run until DMA_CTRL.START_DMA (a later phase).
 *
 * Failure to start is treated as non-fatal (logged, not propagated) since
 * nothing yet depends on this process being up - it's not wired into any
 * register behavior until the register-relay phase.
 */
bool alchemist_guc_proc_start(AlchemistState *s, Error **errp);
void alchemist_guc_proc_stop(AlchemistState *s);

#endif
