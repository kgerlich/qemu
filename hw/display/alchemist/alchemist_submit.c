/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - command submission
 * (context registration and GuC-scheduled execution)
 *
 * Handles the three CTB actions that drive real command submission -
 * transcribed directly from xe_guc_submit.c:
 *
 * 1. GUC_ACTION_REGISTER_CONTEXT (__register_exec_queue()): records a
 *    guc_id's LRC (Logical Ring Context) GGTT address so a later
 *    SCHED_CONTEXT[_MODE_SET] can find its ring.
 *
 * 2. XE_GUC_ACTION_SCHED_CONTEXT_MODE_SET / XE_GUC_ACTION_SCHED_CONTEXT
 *    (submit_exec_queue()): the real "go run this context's ring" trigger.
 *    Both are sent as HXG_TYPE_FAST_REQUEST - xe_guc_ct.c's h2g_write()
 *    never sets want_response for either, so no synchronous CTB reply is
 *    expected (see alchemist_ctb.c's file comment).
 *
 * All three are silently ignored (correctly - not a bug) for any guc_id
 * we haven't seen a REGISTER_CONTEXT for, or when the ring's fixed
 * completion epilogue (see below) isn't found where expected - a
 * malformed/unexpected ring is left alone, not guessed at.
 *
 * We don't interpret the ring/batch content in general - only the
 * fixed 9-dword tail every render/compute-class job ends with
 * (xe_ring_ops.c emit_job_gen12_render_compute(): a QW PIPE_CONTROL
 * breadcrumb write via emit_pipe_imm_ggtt(), then emit_user_interrupt())
 * is recognized. That's sufficient to signal completion the same way
 * real hardware's completion breadcrumb does, without needing a general
 * MI-instruction command-streamer for the (data-driven, per-workaround,
 * not literally fixed) content that precedes it. RING_TAIL is QWORD-
 * aligned, so this 36-byte (non-QWORD-multiple) epilogue is sometimes
 * followed by a single MI_NOOP pad dword before tail - confirmed live
 * (see docs/alchemist-bringup.md) and tolerated by searching backward
 * for the epilogue's real last instruction rather than assuming zero
 * padding.
 *
 * Other engine classes (xe_ring_ops.c's ring_ops_gen12_copy/_video/_gsc)
 * use their own, different completion sequences (no PIPE_CONTROL at
 * all, e.g. the copy/blitter engine's emit_job_gen12_copy() just uses a
 * plain MI_STORE_DATA_IMM breadcrumb) - not yet recognized here, a
 * distinct follow-on, not a bug in what this file does support.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

static void submit_register_context(AlchemistState *s, const uint32_t *payload,
                                     uint32_t n)
{
    uint32_t guc_id, hwlrca_lo, hwlrca_hi;
    uint64_t hwlrca;

    /* payload: [0]=flags [1]=context_idx [2]=engine_class
     * [3]=engine_submit_mask [4..5]=wq_desc [6..7]=wq_base [8]=wq_size
     * [9]=hwlrca_lo [10]=hwlrca_hi - xe_guc_submit.c __register_exec_queue() */
    if (n < 11) {
        return;
    }

    guc_id = payload[1];
    if (guc_id >= ALCHEMIST_MAX_CONTEXTS) {
        return;
    }

    hwlrca_lo = payload[9];
    hwlrca_hi = payload[10];
    hwlrca = ((uint64_t)hwlrca_hi << 32) | hwlrca_lo;

    s->ctx[guc_id].lrc_ggtt_addr = hwlrca & LRC_DESC_ADDR_MASK;
    s->ctx[guc_id].registered = true;
}

static bool read_ring_dword(AlchemistState *s, uint64_t ring_addr,
                             uint32_t ring_size_bytes, uint32_t byte_off,
                             uint32_t *val)
{
    return alchemist_ggtt_read(s, ring_addr + (byte_off % ring_size_bytes),
                                val, 4);
}

/*
 * Finds and acts on the fixed completion epilogue at the tail of a
 * context's ring - see the file comment. Silently does nothing if the
 * context is unknown or the epilogue isn't the exact pattern expected
 * (no guessing at malformed/unrecognized ring content).
 */
static void submit_run_context(AlchemistState *s, uint32_t guc_id,
                                bool is_mode_set)
{
    uint64_t lrc_addr, regs_off, ring_addr;
    uint32_t tail_reg, ctl_reg, ring_start, ring_size, tail_off;
    uint32_t base, i;
    uint32_t epilogue[MI_EPILOGUE_DWORDS];
    uint64_t seqno_addr;
    uint32_t seqno;

    if (guc_id >= ALCHEMIST_MAX_CONTEXTS || !s->ctx[guc_id].registered) {
        return;
    }

    lrc_addr = s->ctx[guc_id].lrc_ggtt_addr;
    regs_off = lrc_addr + LRC_PPHWSP_SIZE;

    /* Only the tail is needed - the fixed completion epilogue this
     * function looks for always sits immediately before it, regardless
     * of where head currently is. */
    if (!alchemist_ggtt_read(s, regs_off + CTX_RING_TAIL_OFF, &tail_reg, 4) ||
        !alchemist_ggtt_read(s, regs_off + CTX_RING_START_OFF, &ring_start, 4) ||
        !alchemist_ggtt_read(s, regs_off + CTX_RING_CTL_OFF, &ctl_reg, 4)) {
        return;
    }
    ring_addr = ring_start; /* CTX_RING_START is a plain 32-bit GGTT address */

    tail_off = tail_reg & RING_TAIL_ADDR_MASK;
    /* RING_CTL_SIZE(size) = size - PAGE_SIZE, RING_VALID = bit 0 - see
     * regs/xe_engine_regs.h and alchemist_guc.c's DMA_CTRL handling for
     * the same masked-register convention. */
    ring_size = (ctl_reg & ~RING_CTL_VALID) + LRC_PPHWSP_SIZE;
    if (ring_size < MI_EPILOGUE_DWORDS * 4) {
        return;
    }

    /*
     * RING_TAIL is QWORD (8-byte) aligned (RING_TAIL_ADDR_MASK reserves
     * the low 3 bits), but the epilogue is 9 dwords (36 bytes) - not a
     * multiple of 8 - so xe_lrc_write_ring() pads with an MI_NOOP (value
     * 0) to reach that alignment. Search backward from tail for the
     * epilogue's real last instruction (MI_ARB_CHECK) across a small,
     * bounded pad tolerance rather than assuming zero padding.
     */
    base = 0;
    for (i = 0; i <= MI_EPILOGUE_PAD_TOLERANCE; i++) {
        uint32_t last_off = (tail_off + ring_size - 4 - i * 4) % ring_size;
        uint32_t last_val;

        if (!read_ring_dword(s, ring_addr, ring_size, last_off, &last_val)) {
            return;
        }
        if (last_val == MI_ARB_CHECK) {
            base = (last_off + ring_size - (MI_EPILOGUE_DWORDS - 1) * 4) %
                   ring_size;
            break;
        }
        if (i == MI_EPILOGUE_PAD_TOLERANCE) {
            /* No recognizable epilogue within tolerance - leave it
             * alone rather than guess. */
            return;
        }
    }

    for (i = 0; i < MI_EPILOGUE_DWORDS; i++) {
        if (!read_ring_dword(s, ring_addr, ring_size, base + i * 4,
                              &epilogue[i])) {
            return;
        }
    }

    if (epilogue[0] != GFX_OP_PIPE_CONTROL_LEN6 ||
        epilogue[1] != PIPE_CONTROL_BREADCRUMB_FLAGS ||
        epilogue[6] != MI_USER_INTERRUPT) {
        /* Not the epilogue we know how to recognize - leave it alone
         * rather than guess. */
        return;
    }

    seqno_addr = epilogue[2];
    seqno = epilogue[4];

    alchemist_ggtt_write(s, seqno_addr, &seqno, sizeof(seqno));
    alchemist_irq_raise_rcs0(s);

    if (is_mode_set) {
        alchemist_ctb_send_sched_context_mode_done(s, guc_id,
                                                     GUC_CONTEXT_ENABLE);
    }
}

void alchemist_submit_handle_action(AlchemistState *s, uint32_t action,
                                     const uint32_t *payload, uint32_t n)
{
    switch (action) {
    case GUC_ACTION_REGISTER_CONTEXT:
        submit_register_context(s, payload, n);
        break;
    case XE_GUC_ACTION_SCHED_CONTEXT:
        if (n >= 1) {
            submit_run_context(s, payload[0], false);
        }
        break;
    case XE_GUC_ACTION_SCHED_CONTEXT_MODE_SET:
        if (n >= 2 && payload[1] == GUC_CONTEXT_ENABLE) {
            submit_run_context(s, payload[0], true);
        }
        break;
    default:
        break;
    }
}
