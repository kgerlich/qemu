/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - PCODE mailbox handshake
 *
 * The real protocol (see xe_pcode.c / xe_pcode_ready() upstream): the
 * guest writes its request into PCODE_DATA0/DATA1, then writes
 * PCODE_MAILBOX = PCODE_READY | mbox. Hardware processes the request,
 * clears PCODE_READY, and leaves the low byte of PCODE_MAILBOX as an
 * error/status code (0 = success) with any reply data in PCODE_DATA0.
 * The guest polls PCODE_MAILBOX until PCODE_READY clears - on real
 * hardware xe_pcode_ready() gives this up to 3 minutes before giving up.
 *
 * We only answer the exact request xe_pcode_ready() sends during probe
 * (DGFX_GET_INIT_STATUS on the DGFX_PCODE_STATUS mailbox). Anything else
 * is deliberately left with PCODE_READY still set rather than blanket-
 * acknowledged: an unexpected mailbox command stalling out is useful
 * evidence of what the next phase needs to implement, not something to
 * paper over.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

void alchemist_pcode_mmio_write(AlchemistState *s, hwaddr addr, unsigned size)
{
    uint32_t mailbox, mbox_cmd, data0;

    if (addr != ALCHEMIST_REG_PCODE_MAILBOX || size != 4) {
        return;
    }

    mailbox = alchemist_mmio_load32(s, ALCHEMIST_REG_PCODE_MAILBOX);
    if (!(mailbox & PCODE_READY)) {
        return;
    }

    mbox_cmd = mailbox & PCODE_MB_COMMAND;
    data0 = alchemist_mmio_load32(s, ALCHEMIST_REG_PCODE_DATA0);

    if (mbox_cmd == DGFX_PCODE_STATUS && data0 == DGFX_GET_INIT_STATUS) {
        alchemist_mmio_store32(s, ALCHEMIST_REG_PCODE_DATA0,
                                DGFX_INIT_STATUS_COMPLETE);
        alchemist_mmio_store32(s, ALCHEMIST_REG_PCODE_MAILBOX, PCODE_SUCCESS);
    }
}
