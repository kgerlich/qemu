/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - GPGPU/compute dispatch
 *
 * Extends alchemist_submit.c's ring handling to XE_ENGINE_CLASS_COMPUTE:
 * real compute command content (PIPELINE_SELECT/STATE_BASE_ADDRESS/
 * CFE_STATE/COMPUTE_WALKER) never appears directly in the ring - both
 * xe_ring_ops.c and Intel compute-runtime's own command-container code
 * confirm it always lives in a separate, PPGTT- or GGTT-addressed
 * indirect batch buffer, reached from the ring via
 * MI_BATCH_BUFFER_START. So this walks forward from the ring's current
 * head to find that jump, then walks the batch itself looking for
 * STATE_BASE_ADDRESS (to capture the Instruction Base Address a
 * COMPUTE_WALKER's kernel-start pointer is relative to) and
 * COMPUTE_WALKER (to actually dispatch).
 *
 * Both walkers use the same generic MI/GFXPIPE instruction-length rule
 * (gpgpu_instr_length()) to skip content they don't act on, rather than
 * assuming a fixed intermediate instruction count - real command streams
 * (e.g. DG2's render-cache-flush workaround) insert a variable number of
 * extra dwords between MI_BATCH_BUFFER_START and the commands that
 * matter here, so a fixed offset would be wrong, not just imprecise.
 *
 * This phase's whole scope is a single 1x1x1-thread-group, 1-thread
 * COMPUTE_WALKER whose entire cross-thread payload fits in the 32 bytes
 * of "Inline Data" COMPUTE_WALKER carries directly (no indirect payload
 * fetch) - the real minimal OpenCL dispatch shape (Phase 13 research).
 * Anything else (multiple threads/groups, indirect payload, more than
 * one COMPUTE_WALKER per batch) is left alone, not guessed at, exactly
 * the same discipline alchemist_submit.c already uses for engine
 * classes/epilogue shapes it doesn't recognize.
 *
 * Completion signaling is deliberately NOT handled here: compute reuses
 * exactly the same ring epilogue/seqno/interrupt mechanism as render
 * (xe_ring_ops.c: XE_ENGINE_CLASS_COMPUTE and _RENDER both resolve to
 * emit_job_gen12_render_compute()) - see alchemist_submit.c's
 * submit_run_context(), which calls alchemist_gpgpu_process_ring() first
 * (so any EU-thread memory write happens before completion is signaled,
 * matching real causality) and then runs its own existing epilogue
 * search unchanged.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"
#include "alchemist_eu.h"

/* Generic MI/GFXPIPE instruction length, in dwords, from its header
 * dword alone - the small set of fixed-single-dword MI opcodes and
 * PIPELINE_SELECT (gen125.xml: bias="1" length="1", no runtime length
 * field at all) are special cases; everything else follows the
 * documented bias-2 "length field = dwords - 2" convention. Returns 0
 * for a command type we don't recognize at all - callers stop walking
 * rather than guess. */
static uint32_t gpgpu_instr_length(uint32_t hdr)
{
    uint32_t type = (hdr >> 29) & 0x7u;

    if (type == GPGPU_CMD_TYPE_MI) {
        uint32_t mi_opcode = (hdr >> 23) & 0x3Fu;

        switch (mi_opcode) {
        case 0x0:  /* MI_NOOP */
        case 0x2:  /* MI_USER_INTERRUPT */
        case 0x5:  /* MI_ARB_CHECK */
        case 0x8:  /* MI_ARB_ON_OFF */
        case 0xA:  /* MI_BATCH_BUFFER_END */
            return 1;
        default:
            return (hdr & 0xFFu) + 2;
        }
    }
    if (type == GPGPU_CMD_TYPE_GFXPIPE) {
        if ((hdr & PIPELINE_SELECT_HDR_MASK) == PIPELINE_SELECT_HDR_VALUE) {
            return 1;
        }
        return (hdr & 0xFFu) + 2;
    }
    return 0;
}

/* Which address space a batch/indirect address is in - set once from
 * MI_BATCH_BUFFER_START's own ppgtt-flag bit (xe_mi_commands.h), since
 * everything reached through that jump lives in the same space it
 * pointed into. */
typedef struct GpgpuMem {
    AlchemistState *s;
    uint32_t guc_id;
    bool use_ppgtt;
} GpgpuMem;

static bool gpgpu_mem_read(const GpgpuMem *mem, uint64_t addr, void *buf,
                            uint64_t len)
{
    return mem->use_ppgtt
               ? alchemist_ppgtt_read(mem->s, mem->guc_id, addr, buf, len)
               : alchemist_ggtt_read(mem->s, addr, buf, len);
}

/*
 * Honors a dispatched thread's terminal send - real compute-runtime/IGC
 * code always ends a kernel with an LSC message (a real OpenCL
 * `buf[idx] = val`-style store, or just the EOT-only send.gtwy an empty
 * kernel/one with no side effects emits), so `send` is the only place
 * "did the kernel do anything to memory" is decided.
 *
 * Bit layout and register conventions hardware-verified against a real
 * ocloc-compiled `buf[0] = 42` kernel (see alchemist_regs.h's LSC_*
 * comment and docs/alchemist-bringup.md): a flat/stateless (addr_type
 * FLAT, addr_size A64) scalar 32-bit (data_size D32, vect_size V1) store
 * carries its 8-byte GPU VA in the GRF at `send->payload_reg` (src0) and
 * its 4-byte data value in the GRF immediately after the address
 * payload - `payload_reg + msg_length` (desc bits [28:25], the address
 * payload's own length in GRFs), not a hardcoded +1, since msg_length is
 * real, address-size-dependent hardware behavior, not a fixed offset
 * that happens to match this one case.
 *
 * Anything else (a different op, non-flat/non-A64/non-D32/non-V1 shape,
 * a different SFID entirely) is left alone, not guessed at - the same
 * discipline the rest of this project uses for unrecognized content.
 * EOT-only sends (SFID Message Gateway) naturally fall through here with
 * no memory effect, which is correct: they have none on real hardware.
 */
static void gpgpu_handle_send(const GpgpuMem *mem, const AlchemistEuState *regs,
                               const AlchemistEuSend *send)
{
    uint32_t desc, op, addr_type, addr_size, data_size, vect_size, msg_length;
    uint32_t data_reg;
    uint64_t addr;
    uint32_t data;

    if (send->sfid != EU_SFID_UGM || send->desc_is_reg) {
        return;
    }

    desc = send->desc;
    op = desc & LSC_DESC_OP_MASK;
    addr_type = (desc >> LSC_DESC_ADDR_TYPE_SHIFT) & LSC_DESC_ADDR_TYPE_MASK;
    addr_size = (desc >> LSC_DESC_ADDR_SIZE_SHIFT) & LSC_DESC_ADDR_SIZE_MASK;
    data_size = (desc >> LSC_DESC_DATA_SIZE_SHIFT) & LSC_DESC_DATA_SIZE_MASK;
    vect_size = (desc >> LSC_DESC_VECT_SIZE_SHIFT) & LSC_DESC_VECT_SIZE_MASK;
    msg_length = (desc >> LSC_DESC_MSG_LENGTH_SHIFT) & LSC_DESC_MSG_LENGTH_MASK;

    if (op != LSC_OP_STORE || addr_type != LSC_ADDR_SURFTYPE_FLAT ||
        addr_size != LSC_ADDR_SIZE_A64 || data_size != LSC_DATA_SIZE_D32 ||
        vect_size != LSC_VECT_SIZE_V1) {
        return;
    }

    if (send->payload_reg >= 128 || msg_length >= 128 ||
        send->payload_reg + msg_length >= 128) {
        return;
    }

    memcpy(&addr, regs->grf[send->payload_reg], sizeof(addr));
    data_reg = send->payload_reg + msg_length;
    memcpy(&data, regs->grf[data_reg], sizeof(data));

    alchemist_ppgtt_write(mem->s, mem->guc_id, addr, &data, sizeof(data));
}

/* Decodes and dispatches the COMPUTE_WALKER at `walker_addr`. Scoped to
 * exactly one 1x1x1 thread group / one thread with an inline-only
 * cross-thread payload - the real minimal OpenCL dispatch shape (Phase
 * 13 research); anything else is left alone (real, new scope, not
 * guessed at). */
static void gpgpu_dispatch_walker(const GpgpuMem *mem, uint64_t walker_addr,
                                   uint64_t instr_base_addr)
{
    uint32_t cw[COMPUTE_WALKER_DWORDS];
    uint32_t i, group_x, group_y, group_z, threads_in_group, exec_ctrl;
    uint64_t kernel_addr;
    AlchemistEuState regs;
    uint8_t code[GPGPU_MAX_KERNEL_INSTRS * 16];
    uint32_t n_fetched;
    AlchemistEuSend send;
    AlchemistEuStatus status;

    for (i = 0; i < COMPUTE_WALKER_DWORDS; i++) {
        if (!gpgpu_mem_read(mem, walker_addr + i * 4, &cw[i], 4)) {
            return;
        }
    }

    group_x = cw[CW_DW_GROUP_DIM_X];
    group_y = cw[CW_DW_GROUP_DIM_Y];
    group_z = cw[CW_DW_GROUP_DIM_Z];
    threads_in_group = cw[CW_DW_IDD_THREADS_IN_GROUP] & CW_THREADS_IN_GROUP_MASK;
    exec_ctrl = cw[CW_DW_EXEC_CONTROL];

    if (group_x != 1 || group_y != 1 || group_z != 1 || threads_in_group != 1) {
        return;
    }

    kernel_addr = instr_base_addr +
                  (cw[CW_DW_IDD_KERNEL_START] & CW_KERNEL_START_MASK);

    for (n_fetched = 0; n_fetched < GPGPU_MAX_KERNEL_INSTRS; n_fetched++) {
        if (!gpgpu_mem_read(mem, kernel_addr + n_fetched * 16,
                             &code[n_fetched * 16], 16)) {
            break;
        }
    }
    if (n_fetched == 0) {
        return;
    }

    memset(&regs, 0, sizeof(regs));
    /* r0.4 low byte = thread index within its dispatch group - always 0,
     * the only case handled here (threads_in_group == 1 above).
     * r0.0[31:6] = IndirectDataStartAddress - not needed when the whole
     * cross-thread payload fits inline (also the only case handled
     * here, gated by CW_EMIT_INLINE_PARAMETER below). */

    if (exec_ctrl & CW_EMIT_INLINE_PARAMETER) {
        /* Inline Data (32 bytes) lands at r1 - confirmed against real
         * ocloc-compiled disassembly reading the argument straight from
         * r1 with no memory fetch (Phase 13 research). */
        memcpy(regs.grf[1], &cw[CW_DW_INLINE_DATA_START], 32);
    }

    alchemist_eu_run(&regs, code, n_fetched, &send, &status);

    if (status != ALCHEMIST_EU_SEND) {
        return; /* didn't reach a real send - nothing to honor */
    }

    gpgpu_handle_send(mem, &regs, &send);
}

/* Walks a batch buffer starting at `batch_addr` looking for a nested
 * MI_BATCH_BUFFER_START (real compute-runtime command buffers open with a
 * setup/flush PIPE_CONTROL, then jump into a second, "real" buffer
 * carrying PIPELINE_SELECT/STATE_BASE_ADDRESS/CFE_STATE/COMPUTE_WALKER -
 * hardware-verified live, see docs/alchemist-bringup.md),
 * STATE_BASE_ADDRESS (to capture Instruction Base Address), and
 * COMPUTE_WALKER (to dispatch). Stops at MI_BATCH_BUFFER_END, after
 * dispatching one COMPUTE_WALKER (this milestone's whole scope), or on
 * anything unrecognized (gpgpu_instr_length() returning 0) - no guessing
 * at malformed/unsupported batch content. A nested jump gets its own
 * independently-read PPGTT-flag bit (not inherited from the outer batch -
 * real MI_BATCH_BUFFER_START always carries its own) and is followed via
 * recursion, `instr_base_addr` propagated forward since STATE_BASE_ADDRESS
 * is context-wide state, not batch-local; `depth` bounds recursion against
 * malformed/adversarial content chaining jumps indefinitely
 * (GPGPU_MAX_BATCH_NESTING). */
static void gpgpu_process_batch(const GpgpuMem *mem, uint64_t batch_addr,
                                 uint64_t instr_base_addr, uint32_t depth)
{
    uint64_t pos = batch_addr;
    uint32_t guard;

    if (depth >= GPGPU_MAX_BATCH_NESTING) {
        return;
    }

    for (guard = 0; guard < GPGPU_BATCH_WALK_GUARD; guard++) {
        uint32_t hdr, len;

        if (!gpgpu_mem_read(mem, pos, &hdr, 4)) {
            return;
        }

        if ((hdr & MI_OPCODE_HDR_MASK) == MI_BATCH_BUFFER_END_HDR_VALUE) {
            return;
        }

        if ((hdr & MI_OPCODE_HDR_MASK) == MI_BATCH_BUFFER_START_HDR_VALUE) {
            uint32_t lo, hi;

            if (gpgpu_mem_read(mem, pos + 4, &lo, 4) &&
                gpgpu_mem_read(mem, pos + 8, &hi, 4)) {
                GpgpuMem nested = {
                    mem->s, mem->guc_id,
                    (hdr & MI_BATCH_BUFFER_START_PPGTT_FLAG) != 0
                };
                uint64_t nested_addr = ((uint64_t)hi << 32) | lo;

                gpgpu_process_batch(&nested, nested_addr, instr_base_addr,
                                     depth + 1);
            }
            return;
        }

        if ((hdr & STATE_BASE_ADDRESS_HDR_MASK) == STATE_BASE_ADDRESS_HDR_VALUE) {
            uint32_t lo, hi;

            if (gpgpu_mem_read(mem, pos + STATE_BASE_ADDRESS_INSTR_BASE_DW * 4,
                                &lo, 4) &&
                gpgpu_mem_read(mem,
                                pos + (STATE_BASE_ADDRESS_INSTR_BASE_DW + 1) * 4,
                                &hi, 4) &&
                (lo & STATE_BASE_ADDRESS_INSTR_BASE_MODIFY_EN)) {
                instr_base_addr = (((uint64_t)hi << 32) | lo) &
                                   0xFFFFFFFFFFFFF000ull;
            }
        } else if ((hdr & GPGPU_CMD_HDR_MASK) == COMPUTE_WALKER_HDR_VALUE) {
            gpgpu_dispatch_walker(mem, pos, instr_base_addr);
            return;
        }

        len = gpgpu_instr_length(hdr);
        if (len == 0) {
            return;
        }
        pos += (uint64_t)len * 4;
    }
}

/*
 * Walks a context's ring from `head` to `tail` looking for
 * MI_BATCH_BUFFER_START - real compute command content lives in the
 * indirect batch it jumps to, never directly in the ring (see the file
 * comment). Called from alchemist_submit.c's submit_run_context() for
 * XE_ENGINE_CLASS_COMPUTE contexts, before its existing epilogue/
 * completion-signaling logic runs.
 */
void alchemist_gpgpu_process_ring(AlchemistState *s, uint32_t guc_id,
                                   uint64_t ring_addr, uint32_t ring_size,
                                   uint32_t head, uint32_t tail)
{
    uint32_t pos = head;
    uint32_t guard;

    for (guard = 0; pos != tail && guard < GPGPU_RING_WALK_GUARD; guard++) {
        uint32_t hdr;

        if (!alchemist_ggtt_read(s, ring_addr + (pos % ring_size), &hdr, 4)) {
            return;
        }

        if ((hdr & MI_OPCODE_HDR_MASK) == MI_BATCH_BUFFER_START_HDR_VALUE) {
            uint32_t lo, hi;

            if (alchemist_ggtt_read(s, ring_addr + ((pos + 4) % ring_size),
                                     &lo, 4) &&
                alchemist_ggtt_read(s, ring_addr + ((pos + 8) % ring_size),
                                     &hi, 4)) {
                GpgpuMem mem = {
                    s, guc_id,
                    (hdr & MI_BATCH_BUFFER_START_PPGTT_FLAG) != 0
                };
                uint64_t batch_addr = ((uint64_t)hi << 32) | lo;

                gpgpu_process_batch(&mem, batch_addr, 0, 0);
            }
            return;
        }

        {
            uint32_t len = gpgpu_instr_length(hdr);

            if (len == 0) {
                return;
            }
            pos += len * 4;
        }
    }
}
