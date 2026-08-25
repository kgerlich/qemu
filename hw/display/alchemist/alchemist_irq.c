/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - GT interrupt cascade
 *
 * Real Intel GPUs route interrupts through several indirection levels
 * rather than a flat status register - transcribed directly from
 * xe_irq.c (dg1_irq_handler/gt_irq_handler/gt_engine_identity) and
 * regs/xe_irq_regs.h:
 *
 *  DG1_MSTR_TILE_INTR (top-level "does any tile have something pending")
 *    -> GFX_MSTR_IRQ (per-tile: which category - GT banks, display, ...)
 *      -> GT_INTR_DW(bank) (per-bank: which specific source, e.g. GuC)
 *        -> IIR_REG_SELECTOR(bank)/INTR_IDENTITY_REG(bank): the driver
 *           writes which bit it wants identified, and polls for us to
 *           respond with an encoded class/instance/vector - this exists
 *           on real hardware so one status register can serve many
 *           possible sources without a dedicated register per source.
 *
 * We only ever raise three sources, all bank 0 - GuC2Host, RCS0 (render
 * engine completion) and BCS0 (blitter/copy engine completion), both
 * from alchemist_submit.c - since no other engines are modeled yet, but
 * the mechanism is implemented for real: the driver walks this exact
 * cascade
 * regardless of how many sources exist behind it, so a shortcut here
 * (e.g. firing the MSI without the identity chain backing it) would
 * leave the driver's own gt_engine_identity() spinning until its ~100us
 * timeout and logging a real error, not a working interrupt.
 *
 * GT_INTR_DW/GFX_MSTR_IRQ/DG1_MSTR_TILE_INTR are all write-1-to-clear
 * (standard Intel ISR/IIR convention, and confirmed directly against
 * dg1_intr_disable()'s write(0)-then-read-then-writeback pattern: a
 * write of 0 changes nothing, which is exactly "sample the current
 * level" - not a special case, just what W1C-with-nothing-to-clear
 * already means). Because of that these three registers are handled
 * entirely here rather than via the generic store-then-react pattern
 * the other modules use - see alchemist.c's dispatch.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/pci/msi.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

bool alchemist_irq_is_status_reg(hwaddr addr)
{
    return addr == ALCHEMIST_REG_DG1_MSTR_TILE_INTR ||
           addr == ALCHEMIST_REG_GFX_MSTR_IRQ ||
           addr == ALCHEMIST_REG_GT_INTR_DW(0) ||
           addr == ALCHEMIST_REG_GT_INTR_DW(1);
}

/* Common to every bank-0 source (GuC2Host, RCS0, ...): set the source's
 * own status bit, then the two cascade levels above it, then fire the
 * actual MSI - see the file comment for why each level is real, not a
 * shortcut. */
static void alchemist_irq_raise_gt0(AlchemistState *s, uint32_t bit)
{
    uint32_t v;

    v = alchemist_mmio_load32(s, ALCHEMIST_REG_GT_INTR_DW(0));
    alchemist_mmio_store32(s, ALCHEMIST_REG_GT_INTR_DW(0), v | bit);

    v = alchemist_mmio_load32(s, ALCHEMIST_REG_GFX_MSTR_IRQ);
    alchemist_mmio_store32(s, ALCHEMIST_REG_GFX_MSTR_IRQ,
                            v | MASTER_IRQ | GT_DW_IRQ0);

    v = alchemist_mmio_load32(s, ALCHEMIST_REG_DG1_MSTR_TILE_INTR);
    alchemist_mmio_store32(s, ALCHEMIST_REG_DG1_MSTR_TILE_INTR,
                            v | DG1_MSTR_IRQ | DG1_MSTR_TILE0);

    msi_notify(&s->pdev, 0);
}

void alchemist_irq_raise_guc2host(AlchemistState *s)
{
    alchemist_irq_raise_gt0(s, INTR_GUC);
}

void alchemist_irq_raise_rcs0(AlchemistState *s)
{
    alchemist_irq_raise_gt0(s, INTR_RCS0);
}

void alchemist_irq_raise_bcs0(AlchemistState *s)
{
    alchemist_irq_raise_gt0(s, INTR_BCS0);
}

/* GT_INTR_DW/GFX_MSTR_IRQ/DG1_MSTR_TILE_INTR: write-1-to-clear status
 * registers, handled here instead of via the generic buffer store - see
 * the file comment above for why. Called from alchemist_mmio_write()
 * *instead of* the generic memcpy for these three addresses. */
void alchemist_irq_status_write(AlchemistState *s, hwaddr addr, uint64_t val)
{
    uint32_t old = alchemist_mmio_load32(s, addr);

    alchemist_mmio_store32(s, addr, old & ~(uint32_t)val);
}

/* IIR_REG_SELECTOR(bank): the guest writes BIT(bit) to ask "what is
 * source `bit` in this bank" and polls INTR_IDENTITY_REG(bank) for our
 * answer. This one *is* a plain (non-W1C) register - our answer goes
 * through the ordinary generic-store-then-react dispatch. */
void alchemist_irq_mmio_write(AlchemistState *s, hwaddr addr, unsigned size)
{
    unsigned bank;
    uint32_t selector, identity;

    if (size != 4) {
        return;
    }

    for (bank = 0; bank < 2; bank++) {
        if (addr == ALCHEMIST_REG_IIR_REG_SELECTOR(bank)) {
            selector = alchemist_mmio_load32(s, addr);

            if (bank == 0 && selector == INTR_GUC) {
                identity = INTR_DATA_VALID |
                           (XE_ENGINE_CLASS_OTHER << INTR_ENGINE_CLASS_SHIFT) |
                           (OTHER_GUC_INSTANCE << INTR_ENGINE_INSTANCE_SHIFT) |
                           GUC_INTR_GUC2HOST;
                alchemist_mmio_store32(s, ALCHEMIST_REG_INTR_IDENTITY_REG(bank),
                                        identity);
            } else if (bank == 0 && selector == INTR_RCS0) {
                identity = INTR_DATA_VALID |
                           (XE_ENGINE_CLASS_RENDER << INTR_ENGINE_CLASS_SHIFT) |
                           (0 << INTR_ENGINE_INSTANCE_SHIFT) |
                           GT_MI_USER_INTERRUPT;
                alchemist_mmio_store32(s, ALCHEMIST_REG_INTR_IDENTITY_REG(bank),
                                        identity);
            } else if (bank == 0 && selector == INTR_BCS0) {
                identity = INTR_DATA_VALID |
                           (XE_ENGINE_CLASS_COPY << INTR_ENGINE_CLASS_SHIFT) |
                           (0 << INTR_ENGINE_INSTANCE_SHIFT) |
                           GT_MI_USER_INTERRUPT;
                alchemist_mmio_store32(s, ALCHEMIST_REG_INTR_IDENTITY_REG(bank),
                                        identity);
            }
            /*
             * Any other selector value has no known source behind it
             * (we don't model engine interrupts yet) - leave
             * INTR_IDENTITY_REG without INTR_DATA_VALID set, exactly as
             * real hardware would for a source that isn't there. The
             * driver's own gt_engine_identity() already tolerates this
             * (a ~100us poll timeout and a logged error), not a hang.
             */
            return;
        }
    }
}
