/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - GuC firmware load handshake
 *
 * Two write-triggered behaviors, both transcribed directly from
 * xe_wopcm.c/xe_uc_fw.c upstream:
 *
 * 1. WOPCM partition registers (xe_wopcm.c __wopcm_init_regs()): the guest
 *    writes the computed size/offset and then reads the register back,
 *    expecting hardware to have set a "locked"/"valid" status bit as
 *    confirmation. GUC_WOPCM_SIZE bit 0 and DMA_GUC_WOPCM_OFFSET bit 0 are
 *    that confirmation bit - we set them the moment the guest writes,
 *    since there's no real WOPCM hardware state to actually lock.
 *
 * 2. DMA_CTRL (xe_uc_fw.c uc_fw_xfer()): a masked-write register (bits
 *    [31:16] select which of bits [15:0] to update, same convention as
 *    forcewake - see alchemist_forcewake.c) that starts a firmware DMA
 *    transfer when START_DMA is set. Real hardware clears START_DMA once
 *    the transfer completes and the guest polls for that. We apply the
 *    masked update, immediately clear START_DMA (there's nothing to
 *    actually transfer - the guest's own GGTT-mapped source and WOPCM
 *    destination are both within guest memory we don't need to touch),
 *    and report GUC_STATUS as booted and authenticated. As with PCODE
 *    (see alchemist_pcode.c), the driver never itself verifies the
 *    firmware cryptographically - that's delegated to hardware, which is
 *    us, and reporting success here isn't a shortcut around real
 *    driver-side validation.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

void alchemist_guc_mmio_write(AlchemistState *s, hwaddr addr, unsigned size)
{
    if (size != 4) {
        return;
    }

    if (addr == ALCHEMIST_REG_GUC_WOPCM_SIZE) {
        uint32_t val = alchemist_mmio_load32(s, addr);
        alchemist_mmio_store32(s, addr, val | GUC_WOPCM_SIZE_LOCKED);
        return;
    }

    if (addr == ALCHEMIST_REG_DMA_GUC_WOPCM_OFFSET) {
        uint32_t val = alchemist_mmio_load32(s, addr);
        alchemist_mmio_store32(s, addr, val | GUC_WOPCM_OFFSET_VALID);
        return;
    }

    if (addr == ALCHEMIST_REG_DMA_CTRL) {
        uint32_t ctl = alchemist_mmio_load32(s, addr);
        uint32_t mask = (ctl >> 16) & 0xFFFFu;
        uint32_t data = ctl & 0xFFFFu;
        uint32_t new_state = data & mask;

        if (!(mask & START_DMA) || !(data & START_DMA)) {
            alchemist_mmio_store32(s, addr, new_state);
            return;
        }

        /* DMA "completes" immediately: clear START_DMA, report GuC booted. */
        alchemist_mmio_store32(s, addr, new_state & ~START_DMA);
        alchemist_mmio_store32(s, ALCHEMIST_REG_GUC_STATUS,
                                GS_AUTH_STATUS_GOOD | GS_UKERNEL_READY |
                                GS_BOOTROM_JUMP_PASSED);
        return;
    }
}
