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
 * We don't interpret the ring/batch content in general - only the fixed
 * tail every job ends with is recognized, and which shape to expect is
 * looked up per-context from the engine_class REGISTER_CONTEXT reported
 * (xe_ring_ops.c's per-class ring_ops table):
 *
 * - XE_ENGINE_CLASS_RENDER and XE_ENGINE_CLASS_COMPUTE (the latter
 *   additionally routed through alchemist_gpgpu.c first - see below):
 *   emit_job_gen12_render_compute(), a 9-dword QW PIPE_CONTROL
 *   breadcrumb write via emit_pipe_imm_ggtt(), then emit_user_interrupt().
 * - XE_ENGINE_CLASS_COPY: __emit_job_gen12_simple(), a different,
 *   shorter 8-dword MI_FLUSH_DW breadcrumb, no PIPE_CONTROL at all.
 *
 * Both endings share the exact same 3-dword emit_user_interrupt()
 * suffix (MI_USER_INTERRUPT, MI_ARB_ON_OFF|ENABLE, MI_ARB_CHECK), so
 * MI_ARB_CHECK - a fixed, recognizable value - is searched for backward
 * from tail as the anchor, then the appropriate epilogue shape is
 * checked ending at that position. RING_TAIL is QWORD (8-byte) aligned;
 * the render/compute epilogue is 36 bytes (not a multiple of 8), so it's
 * sometimes followed by a single MI_NOOP pad dword before tail -
 * confirmed live (see docs/alchemist-bringup.md), which is exactly why
 * this searches for the anchor within a small tolerance instead of
 * assuming a fixed offset (the copy epilogue is 32 bytes, already
 * aligned, but the same tolerant search costs nothing extra and needs
 * no special-casing).
 *
 * Any other engine class is left alone (silently ignored, not guessed
 * at) until real evidence - an actual stall - shows it's needed, the
 * same discipline used throughout this project.
 *
 * XE_ENGINE_CLASS_COMPUTE additionally gets a real forward walk of its
 * ring/indirect-batch content (alchemist_gpgpu_process_ring(), in
 * alchemist_gpgpu.c) before the epilogue search below runs - unlike
 * render/copy, a compute dispatch's actual work (COMPUTE_WALKER) has to
 * be found and executed, not just detected as "present", for the
 * dispatch to mean anything.
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
    s->ctx[guc_id].engine_class = payload[2];
    s->ctx[guc_id].registered = true;
}

static bool read_ring_dword(AlchemistState *s, uint64_t ring_addr,
                             uint32_t ring_size_bytes, uint32_t byte_off,
                             uint32_t *val)
{
    return alchemist_ggtt_read(s, ring_addr + (byte_off % ring_size_bytes),
                                val, 4);
}

/* Reads `count` dwords ending at (and including) `last_off`, i.e.
 * starting `count-1` dwords before it. */
static bool read_epilogue(AlchemistState *s, uint64_t ring_addr,
                           uint32_t ring_size, uint32_t last_off,
                           uint32_t count, uint32_t *out)
{
    uint32_t base = (last_off + ring_size - (count - 1) * 4) % ring_size;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (!read_ring_dword(s, ring_addr, ring_size, base + i * 4,
                              &out[i])) {
            return false;
        }
    }
    return true;
}

static bool submit_epilogue_render_compute(AlchemistState *s,
                                            uint64_t ring_addr,
                                            uint32_t ring_size,
                                            uint32_t last_off,
                                            uint64_t *seqno_addr,
                                            uint32_t *seqno)
{
    uint32_t e[MI_EPILOGUE_DWORDS];

    if (!read_epilogue(s, ring_addr, ring_size, last_off,
                        MI_EPILOGUE_DWORDS, e)) {
        return false;
    }
    if (e[0] != GFX_OP_PIPE_CONTROL_LEN6 ||
        e[1] != PIPE_CONTROL_BREADCRUMB_FLAGS ||
        e[6] != MI_USER_INTERRUPT) {
        return false;
    }
    *seqno_addr = e[2];
    *seqno = e[4];
    return true;
}

static bool submit_epilogue_simple(AlchemistState *s, uint64_t ring_addr,
                                    uint32_t ring_size, uint32_t last_off,
                                    uint64_t *seqno_addr, uint32_t *seqno)
{
    uint32_t e[MI_SIMPLE_EPILOGUE_DWORDS];

    if (!read_epilogue(s, ring_addr, ring_size, last_off,
                        MI_SIMPLE_EPILOGUE_DWORDS, e)) {
        return false;
    }
    if (e[0] != MI_ARB_OFF ||
        e[1] != MI_FLUSH_DW_STOREDW_IMM ||
        e[5] != MI_USER_INTERRUPT) {
        return false;
    }
    *seqno_addr = e[2] & ~(uint64_t)MI_FLUSH_DW_USE_GTT_BIT;
    *seqno = e[4];
    return true;
}

/*
 * Finds and acts on the fixed completion epilogue at the tail of a
 * context's ring - see the file comment. Silently does nothing if the
 * context is unknown, its engine class isn't one we recognize an
 * epilogue shape for, or the epilogue isn't found where expected (no
 * guessing at malformed/unrecognized ring content).
 */
static void submit_run_context(AlchemistState *s, uint32_t guc_id,
                                bool is_mode_set)
{
    uint64_t lrc_addr, regs_off, ring_addr;
    uint32_t tail_reg, head_reg, ctl_reg, ring_start, ring_size;
    uint32_t tail_off, head_off;
    uint32_t i, last_off = 0;
    bool found_anchor = false;
    uint64_t seqno_addr;
    uint32_t seqno;
    void (*raise_irq)(AlchemistState *s);

    if (guc_id >= ALCHEMIST_MAX_CONTEXTS || !s->ctx[guc_id].registered) {
        return;
    }

    lrc_addr = s->ctx[guc_id].lrc_ggtt_addr;
    regs_off = lrc_addr + LRC_PPHWSP_SIZE;

    /* head is only needed for the compute case's forward command-stream
     * walk (alchemist_gpgpu.c) - the fixed completion epilogue every
     * other class looks for always sits immediately before tail,
     * regardless of where head currently is. */
    if (!alchemist_ggtt_read(s, regs_off + CTX_RING_TAIL_OFF, &tail_reg, 4) ||
        !alchemist_ggtt_read(s, regs_off + CTX_RING_HEAD_OFF, &head_reg, 4) ||
        !alchemist_ggtt_read(s, regs_off + CTX_RING_START_OFF, &ring_start, 4) ||
        !alchemist_ggtt_read(s, regs_off + CTX_RING_CTL_OFF, &ctl_reg, 4)) {
        return;
    }
    ring_addr = ring_start; /* CTX_RING_START is a plain 32-bit GGTT address */

    tail_off = tail_reg & RING_TAIL_ADDR_MASK;
    head_off = head_reg & RING_HEAD_ADDR_MASK;
    /* RING_CTL_SIZE(size) = size - PAGE_SIZE, RING_VALID = bit 0 - see
     * regs/xe_engine_regs.h and alchemist_guc.c's DMA_CTRL handling for
     * the same masked-register convention. */
    ring_size = (ctl_reg & ~RING_CTL_VALID) + LRC_PPHWSP_SIZE;
    if (ring_size < MI_EPILOGUE_DWORDS * 4) {
        return;
    }

    /* Compute dispatch content (PIPELINE_SELECT/STATE_BASE_ADDRESS/
     * CFE_STATE/COMPUTE_WALKER, reached via MI_BATCH_BUFFER_START) must
     * be processed - and any EU-thread memory write it causes performed
     * - before completion is signaled below, matching real causality.
     * See alchemist_gpgpu.c's file comment for why this can't be folded
     * into the epilogue search: real compute command content never
     * appears directly in the ring. */
    if (s->ctx[guc_id].engine_class == XE_ENGINE_CLASS_COMPUTE) {
        alchemist_gpgpu_process_ring(s, guc_id, ring_addr, ring_size,
                                      head_off, tail_off);
    }

    /* Both epilogue shapes end in the same MI_ARB_CHECK - find it once,
     * within a small pad tolerance, then try the shape this context's
     * engine class actually uses (see the file comment). */
    for (i = 0; i <= MI_EPILOGUE_PAD_TOLERANCE; i++) {
        uint32_t off = (tail_off + ring_size - 4 - i * 4) % ring_size;
        uint32_t val;

        if (!read_ring_dword(s, ring_addr, ring_size, off, &val)) {
            return;
        }
        if (val == MI_ARB_CHECK) {
            last_off = off;
            found_anchor = true;
            break;
        }
    }
    if (!found_anchor) {
        return;
    }

    switch (s->ctx[guc_id].engine_class) {
    case XE_ENGINE_CLASS_RENDER:
        if (!submit_epilogue_render_compute(s, ring_addr, ring_size, last_off,
                                             &seqno_addr, &seqno)) {
            return;
        }
        raise_irq = alchemist_irq_raise_rcs0;
        break;
    case XE_ENGINE_CLASS_COMPUTE:
        /* Same ring epilogue shape as render - xe_ring_ops.c:
         * XE_ENGINE_CLASS_COMPUTE and _RENDER both resolve to
         * emit_job_gen12_render_compute(); only the interrupt identity
         * differs (INTR_CCS0, not INTR_RCS0). */
        if (!submit_epilogue_render_compute(s, ring_addr, ring_size, last_off,
                                             &seqno_addr, &seqno)) {
            return;
        }
        raise_irq = alchemist_irq_raise_ccs0;
        break;
    case XE_ENGINE_CLASS_COPY:
        if (!submit_epilogue_simple(s, ring_addr, ring_size, last_off,
                                     &seqno_addr, &seqno)) {
            return;
        }
        raise_irq = alchemist_irq_raise_bcs0;
        break;
    default:
        /* Not yet a recognized engine class - leave it alone. */
        return;
    }

    alchemist_ggtt_write(s, seqno_addr, &seqno, sizeof(seqno));
    raise_irq(s);

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
