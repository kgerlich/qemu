/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - GuC firmware load handshake
 * and mmio mailbox
 *
 * Three write-triggered behaviors. The first two (WOPCM, DMA_CTRL) are
 * transcribed directly from xe_wopcm.c/xe_uc_fw.c upstream:
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
 * 3. The GuC mmio mailbox (xe_guc.c xe_guc_mmio_send_recv(), abi/
 *    guc_messages_abi.h "HXG" protocol): once booted, the guest and GuC
 *    exchange request/response messages by writing a request into
 *    VF_SW_FLAG(0..3), then writing GUC_HOST_INTERRUPT (any value, any
 *    write at all "rings the doorbell" - it's not a bit-flag register),
 *    then polling VF_SW_FLAG(0) for a GuC-origin response. We implement
 *    the protocol itself faithfully (real HXG header encoding, real
 *    response handshake), but for XE_GUC_ACTION_GET_HWCONFIG specifically
 *    we only answer the size-query correctly (a nonzero placeholder size -
 *    xe_guc_hwconfig_init() hard-fails on a zero size) and otherwise
 *    just acknowledge success. We deliberately don't deliver real
 *    hwconfig table *content* on the follow-up copy request even though
 *    we now have GGTT translation and could write bytes there: real
 *    hwconfig content encodes real GPU capabilities (EU counts, cache
 *    sizes, ...) we have no authentic values for, and fabricating a
 *    plausible-looking entry would be exactly the kind of invented data
 *    to avoid. The guest's hwconfig buffer is left as freshly-allocated
 *    zeroed memory, which xe_guc_hwconfig_lookup_u32()'s parser handles
 *    safely (reads as "zero attributes", not a crash). Flagged here and
 *    in docs/alchemist-bringup.md as a known, deliberate deferral.
 *
 *    GUC_ACTION_HOST2GUC_SELF_CFG additionally gets its key/value decoded
 *    and handed to alchemist_ctb_register() (alchemist_ctb.c) - this is
 *    how the guest tells us the CTB descriptor/ring GGTT addresses that
 *    module needs.
 *
 * GUC_HOST_INTERRUPT is also the doorbell for CTB ring traffic once the
 * guest has moved on from the mmio mailbox (xe_guc_notify() is reused
 * for both - see alchemist_ctb.c's file comment), so every write here
 * checks the H2G ring regardless of whether the mmio mailbox path found
 * anything: after the mailbox answers a request it overwrites
 * VF_SW_FLAG(0) with a RESPONSE-type message, so a later doorbell ring
 * for a real CTB reason naturally reads as "not a fresh mmio request"
 * and falls through correctly - no separate staleness tracking needed.
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

    if (addr == ALCHEMIST_REG_GDRST) {
        alchemist_mmio_store32(s, addr, 0);
        /* A real GuC-domain reset invalidates the previous boot state -
         * xe_guc_reset() explicitly checks GS_MIA_IN_RESET is now set,
         * and a later reload goes through the normal DMA_CTRL/GUC_STATUS
         * handshake above to boot again. */
        alchemist_mmio_store32(s, ALCHEMIST_REG_GUC_STATUS, GS_MIA_IN_RESET);
        return;
    }

    if (addr == ALCHEMIST_REG_GUC_HOST_INTERRUPT) {
        uint32_t req0 = alchemist_mmio_load32(s, ALCHEMIST_REG_VF_SW_FLAG(0));
        uint32_t type = (req0 >> HXG_MSG_0_TYPE_SHIFT) & HXG_TYPE_MASK;

        if (type == HXG_TYPE_REQUEST) {
            uint32_t action = req0 & HXG_REQUEST_MSG_0_ACTION_MASK;
            uint32_t resp0, data0 = 0;

            if (action == GUC_ACTION_GET_HWCONFIG) {
                uint32_t ggtt_lo = alchemist_mmio_load32(s, ALCHEMIST_REG_VF_SW_FLAG(1));
                uint32_t ggtt_hi = alchemist_mmio_load32(s, ALCHEMIST_REG_VF_SW_FLAG(2));
                uint32_t req_size = alchemist_mmio_load32(s, ALCHEMIST_REG_VF_SW_FLAG(3));

                if (ggtt_lo == 0 && ggtt_hi == 0 && req_size == 0) {
                    /* Size query: must be nonzero or xe_guc_hwconfig_init()
                     * treats it as failure. See the file comment above for
                     * why we don't deliver real table content. */
                    data0 = 12;
                }
            } else if (action == GUC_ACTION_HOST2GUC_SELF_CFG) {
                uint32_t word1 = alchemist_mmio_load32(s, ALCHEMIST_REG_VF_SW_FLAG(1));
                uint32_t word2 = alchemist_mmio_load32(s, ALCHEMIST_REG_VF_SW_FLAG(2));
                uint32_t word3 = alchemist_mmio_load32(s, ALCHEMIST_REG_VF_SW_FLAG(3));
                uint16_t key = (word1 >> 16) & 0xFFFFu;
                uint64_t value = ((uint64_t)word3 << 32) | word2;

                alchemist_ctb_register(s, key, value);
                data0 = 1;
            }

            resp0 = (HXG_ORIGIN_GUC << HXG_MSG_0_ORIGIN_SHIFT) |
                    (HXG_TYPE_RESPONSE_SUCCESS << HXG_MSG_0_TYPE_SHIFT) |
                    (data0 & HXG_RESPONSE_MSG_0_DATA0_MASK);
            alchemist_mmio_store32(s, ALCHEMIST_REG_VF_SW_FLAG(0), resp0);
        }

        alchemist_ctb_check_h2g(s);
        return;
    }
}
