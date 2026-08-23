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

typedef struct AlchemistState {
    PCIDevice pdev;
    MemoryRegion mmio;
    MemoryRegion vram;
    uint8_t *mmio_buf;
    uint8_t *vram_ptr;
    AlchemistCtb h2g;
    AlchemistCtb g2h;
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
void alchemist_irq_mmio_write(AlchemistState *s, hwaddr addr, unsigned size);

/*
 * CTB (Command Transport Buffer) - see alchemist_ctb.c. Called from the
 * GuC mmio mailbox's SELF_CFG handler (alchemist_guc.c) to record a
 * registered ring's GGTT address/size, and from the GUC_HOST_INTERRUPT
 * doorbell handler to check for and process new H2G ring traffic.
 */
void alchemist_ctb_register(AlchemistState *s, uint16_t key, uint64_t val);
void alchemist_ctb_check_h2g(AlchemistState *s);

#endif
