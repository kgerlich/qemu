/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - forcewake domains
 *
 * The real protocol (see xe_force_wake.c upstream): each domain has a
 * control register the guest writes with a masked update (bits [31:16]
 * select which of bits [15:0] to change) and a paired ack register the
 * guest polls until the corresponding bit reflects the requested state.
 * We ack immediately - there's no real power-gated hardware behind this
 * device to model a wake delay for.
 *
 * The domain list and offsets here are transcribed directly from
 * xe_force_wake_init_gt()/xe_force_wake_init_engines() and
 * regs/xe_gt_regs.h, covering every domain DG2-class xe defines (GT,
 * render, per-instance media decode/encode, GSC). There is no separate
 * "blitter" forcewake domain in xe - unlike older i915-generation
 * hardware, DG2's copy engine doesn't have its own domain.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

typedef struct AlchemistForcewakeDomain {
    hwaddr ctl;
    hwaddr ack;
} AlchemistForcewakeDomain;

static const AlchemistForcewakeDomain alchemist_forcewake_domains[] = {
    { ALCHEMIST_REG_FORCEWAKE_GT,             ALCHEMIST_REG_FORCEWAKE_ACK_GT },
    { ALCHEMIST_REG_FORCEWAKE_RENDER,         ALCHEMIST_REG_FORCEWAKE_ACK_RENDER },
    { ALCHEMIST_REG_FORCEWAKE_GSC,            ALCHEMIST_REG_FORCEWAKE_ACK_GSC },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(0), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(0) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(1), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(1) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(2), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(2) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(3), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(3) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(4), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(4) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(5), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(5) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(6), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(6) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(7), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(7) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VEBOX(0), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VEBOX(0) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VEBOX(1), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VEBOX(1) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VEBOX(2), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VEBOX(2) },
    { ALCHEMIST_REG_FORCEWAKE_MEDIA_VEBOX(3), ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VEBOX(3) },
};

static void alchemist_forcewake_apply(AlchemistState *s, hwaddr ctl, hwaddr ack)
{
    uint32_t ctl_val = alchemist_mmio_load32(s, ctl);
    uint32_t mask = (ctl_val >> 16) & 0xFFFFu;
    uint32_t data = ctl_val & 0xFFFFu;
    uint32_t ack_val = alchemist_mmio_load32(s, ack);

    ack_val = (ack_val & ~mask) | (data & mask);
    alchemist_mmio_store32(s, ack, ack_val);
}

void alchemist_forcewake_mmio_write(AlchemistState *s, hwaddr addr, unsigned size)
{
    size_t i;

    if (size != 4) {
        return;
    }

    for (i = 0; i < ARRAY_SIZE(alchemist_forcewake_domains); i++) {
        if (addr == alchemist_forcewake_domains[i].ctl) {
            alchemist_forcewake_apply(s, alchemist_forcewake_domains[i].ctl,
                                       alchemist_forcewake_domains[i].ack);
            return;
        }
    }
}
