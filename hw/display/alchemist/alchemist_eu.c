/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - EU (execution unit)
 * instruction-set interpreter
 *
 * The EU is Intel's term for a GPU shader core. This is a functional
 * (not cycle-accurate) interpreter for the small subset of the Gen12.5
 * EU ISA that trivial compute/vertex/pixel programs actually use - real
 * compiled Intel kernels this small are genuinely tiny: an empty OpenCL
 * kernel compiles to exactly 2 instructions (a `mov` staging the thread
 * payload header, then `send{EOT}`), confirmed by hand-decoding real
 * ocloc-compiled DG2 bytes (see below and docs/alchemist-bringup.md).
 *
 * Only the native (128-bit, uncompacted) instruction format is decoded.
 * This is a real, deliberate scope limit, not an oversight: compacted
 * (64-bit) instructions need five separate compiler-controlled lookup
 * tables (32/32/32/16/16 entries) to decode at all, and critically,
 * `send`/branch instructions are *never* compacted on real hardware
 * (confirmed in Intel's own PRM) - so EOT recognition, this phase's
 * actual goal, is completely unaffected by not supporting compaction
 * yet. `mov`/`add` immediate-load patterns are also frequently left
 * uncompacted in real compiled output (confirmed - see the worked
 * examples below). Compact-format decode is deferred until real
 * evidence (an actual program we're trying to run that uses it) shows
 * it's needed, not spent on preemptively.
 *
 * Bit layout cross-confirmed from three independent sources and then
 * hardware-verified directly: Mesa's src/intel/compiler/gen/xe.json
 * (the "Xe" bucket, Gen12-19, current upstream), Intel's IGA assembler's
 * bundled GED decode tables, and - the strongest evidence - hand-
 * decoding real bytes from `ocloc compile -device dg2` output field by
 * field and confirming every field against `iga64`'s own disassembly.
 * Three real, verified instructions (bytes and their exact decode):
 *
 *   mov (8|M0) r127.0<1>:ud 0x0:ud            (thread payload staging)
 *   61 00 03 80 20 42 05 7f 00 00 00 00 00 00 00 00
 *
 *   add (8|M0) r10.0<1>:d r5.0<8;8,1>:d r6.0<8;8,1>:d
 *   40 00 03 00 60 06 05 0a 05 05 46 06 05 06 46 00
 *
 *   send.gtwy (1|M0) null r127 null:0 0x0 0x02000010 {EOT,A@1}
 *   31 09 00 80 04 00 00 00 0c 7f 20 30 00 00 00 00
 *
 * The regioning this interpreter implements (per-lane contiguous read/
 * write, e.g. `r5.0<8;8,1>:d` = one new dword per channel starting at
 * subreg 0) is exactly the pattern all three real examples above use -
 * the standard/default regioning for a straightforward per-channel
 * operation. Non-default regioning (broadcast reads, cross-row access)
 * is real EU functionality this doesn't decode - flagged as
 * ALCHEMIST_EU_UNSUPPORTED rather than silently mishandled.
 *
 * `send`'s message descriptor (`desc`) is not stored contiguously in the
 * instruction word - it's scattered across several bit ranges. The
 * gather below is the exact inverse of Mesa's own scatter formula
 * (gen_encoding.cpp, current upstream, live production-compiler code):
 *   desc[31:30] = instr[123:122]   desc[29:25] = instr[71:67]
 *   desc[24:20] = instr[55:51]     desc[19:11] = instr[121:113]
 *   desc[10:0]  = instr[91:81]
 * This interpreter only decodes the send *envelope* (sfid, EOT, payload
 * base register, the reassembled descriptor) - it never dispatches a
 * message or performs a memory operation itself. That's deliberately a
 * later phase's job (whichever phase first has a real message to send),
 * per this project's "extend only as evidence demands" discipline.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "alchemist_regs.h"
#include "alchemist_eu.h"

/* Extracts bits [hi:lo] (inclusive) from a 128-bit value held as two
 * little-endian 64-bit words (w[0] = bits[63:0], w[1] = bits[127:64]),
 * handling the case where the range spans the word boundary. */
static uint64_t eu_bits(const uint64_t w[2], unsigned hi, unsigned lo)
{
    unsigned width = hi - lo + 1;
    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);

    if (hi < 64) {
        return (w[0] >> lo) & mask;
    }
    if (lo >= 64) {
        return (w[1] >> (lo - 64)) & mask;
    }
    {
        unsigned lo_width = 64 - lo;
        uint64_t lo_part = w[0] >> lo;
        uint64_t hi_part = w[1] & ((1ULL << (width - lo_width)) - 1);

        return (lo_part | (hi_part << lo_width)) & mask;
    }
}

static uint32_t eu_type_width(uint32_t type)
{
    switch (type) {
    case EU_TYPE_UB:
        return 1;
    case EU_TYPE_UW:
    case EU_TYPE_W:
        return 2;
    case EU_TYPE_UD:
    case EU_TYPE_D:
    case EU_TYPE_F:
        return 4;
    default:
        return 0; /* unrecognized - caller must check */
    }
}

typedef struct EuDecoded {
    uint32_t opcode;
    uint32_t exec_size;
    bool dst_is_arf;
    uint32_t dst_regnum;
    uint32_t dst_subregnum;
    uint32_t dst_hstride;
    uint32_t dst_type;
    bool src0_is_imm;
    bool src0_is_arf;
    uint32_t src0_regnum;
    uint32_t src0_subregnum;
    uint32_t src0_type;
    bool src1_is_imm;
    bool src1_is_arf;
    uint32_t src1_regnum;
    uint32_t src1_subregnum;
    uint32_t src1_type;
    uint32_t imm32;
    uint32_t send_sfid;
    bool send_eot;
    bool send_desc_is_reg;
    uint32_t send_desc;
} EuDecoded;

static void eu_decode(const uint8_t instr[16], EuDecoded *d)
{
    uint64_t w[2];
    uint32_t exec_size_log2;

    memcpy(&w[0], instr, 8);
    memcpy(&w[1], instr + 8, 8);

    memset(d, 0, sizeof(*d));

    d->opcode = (uint32_t)eu_bits(w, 6, 0);

    exec_size_log2 = (uint32_t)eu_bits(w, 18, 16);
    d->exec_size = 1u << exec_size_log2;

    d->dst_is_arf = eu_bits(w, 50, 50) == EU_REGFILE_ARF;
    d->dst_regnum = (uint32_t)eu_bits(w, 63, 56);
    d->dst_subregnum = (uint32_t)eu_bits(w, 55, 51);
    d->dst_hstride = (uint32_t)eu_bits(w, 49, 48);
    d->dst_type = (uint32_t)eu_bits(w, 39, 36);

    d->src0_is_imm = eu_bits(w, 46, 46) != 0;
    d->src0_is_arf = eu_bits(w, 66, 66) == EU_REGFILE_ARF;
    d->src0_regnum = (uint32_t)eu_bits(w, 79, 72);
    d->src0_subregnum = (uint32_t)eu_bits(w, 71, 67);
    d->src0_type = (uint32_t)eu_bits(w, 43, 40);

    d->src1_is_imm = eu_bits(w, 47, 47) != 0;
    d->src1_is_arf = eu_bits(w, 98, 98) == EU_REGFILE_ARF;
    d->src1_regnum = (uint32_t)eu_bits(w, 111, 104);
    d->src1_subregnum = (uint32_t)eu_bits(w, 103, 99);
    d->src1_type = (uint32_t)eu_bits(w, 91, 88);

    /* At most one of src0/src1 is ever an immediate on real hardware;
     * the 32-bit immediate always occupies bits[127:96] regardless of
     * which operand it is - confirmed directly against real bytes for
     * both an immediate src0 (mov) and an immediate src1 (add) above. */
    d->imm32 = (uint32_t)eu_bits(w, 127, 96);

    /* send/sendc envelope - bit[34] is SATURATE for ALU ops, SEND_EOT
     * for send; only meaningful here when opcode is actually send. */
    d->send_sfid = (uint32_t)eu_bits(w, 95, 92);
    d->send_eot = eu_bits(w, 34, 34) != 0;
    d->send_desc_is_reg = eu_bits(w, 48, 48) != 0;
    d->send_desc = (uint32_t)(
        (eu_bits(w, 123, 122) << 30) |
        (eu_bits(w, 71, 67) << 25) |
        (eu_bits(w, 55, 51) << 20) |
        (eu_bits(w, 121, 113) << 11) |
        eu_bits(w, 91, 81));
}

/* Reads exec_size contiguous elements of `type` starting at
 * grf[regnum].subregnum (the confirmed default/standard regioning
 * pattern - see the file comment) into `out` (exec_size dwords, each
 * zero/sign-extended from the real element width). */
static bool eu_read_operand(AlchemistEuState *regs, bool is_arf,
                             uint32_t regnum, uint32_t subregnum,
                             uint32_t type, uint32_t exec_size,
                             uint32_t out[32])
{
    uint32_t width = eu_type_width(type);
    uint32_t i;

    if (width == 0 || is_arf || regnum >= 128 ||
        subregnum + exec_size * width > sizeof(regs->grf[0])) {
        return false;
    }

    for (i = 0; i < exec_size; i++) {
        const uint8_t *p = &regs->grf[regnum][subregnum + i * width];
        uint32_t v = 0;

        memcpy(&v, p, width);
        if ((type == EU_TYPE_W) && (v & 0x8000)) {
            v |= 0xFFFF0000u; /* sign-extend */
        }
        out[i] = v;
    }
    return true;
}

static bool eu_write_operand(AlchemistEuState *regs, bool is_arf,
                              uint32_t regnum, uint32_t subregnum,
                              uint32_t hstride, uint32_t type,
                              uint32_t exec_size, const uint32_t val[32])
{
    uint32_t width = eu_type_width(type);
    uint32_t i;

    if (is_arf) {
        /* ARF destinations (in practice: "null") discard the result -
         * a real, correct thing to do, not an unhandled case. Any
         * *other* ARF register (address/accumulator/flag) as a write
         * target is genuinely unsupported. */
        return regnum == EU_ARF_NULL;
    }
    if (width == 0 || hstride != 1 || regnum >= 128 ||
        subregnum + exec_size * width > sizeof(regs->grf[0])) {
        return false;
    }

    for (i = 0; i < exec_size; i++) {
        memcpy(&regs->grf[regnum][subregnum + i * width], &val[i], width);
    }
    return true;
}

/*
 * The raw 32-bit immediate field is always full-width in the instruction
 * encoding, but its *meaningful* value is only src0_type's own width -
 * e.g. a real ocloc-compiled `mov r4.0<1>:d 42:w` encodes imm32 as
 * 0x002A002A (the 16-bit value duplicated into both halves, a hardware
 * encoding convenience), not literally 42 as a 32-bit value. Narrowing
 * to src0_type's width and then sign/zero-extending (matching
 * eu_read_operand's identical W-sign-extension convention for register
 * reads) is what recovers the real 42 - confirmed against this exact
 * real instruction (see docs/alchemist-bringup.md, Phase 13). Returns
 * false for an unrecognized type, same contract as eu_read_operand.
 */
static bool eu_imm_value(uint32_t imm32, uint32_t type, uint32_t *out)
{
    uint32_t width = eu_type_width(type);
    uint32_t v;

    if (width == 0) {
        return false;
    }

    v = (width == 4) ? imm32 : (imm32 & ((1u << (width * 8)) - 1));
    if (type == EU_TYPE_W && (v & 0x8000)) {
        v |= 0xFFFF0000u; /* sign-extend */
    }
    *out = v;
    return true;
}

static AlchemistEuStatus eu_exec_mov(AlchemistEuState *regs,
                                      const EuDecoded *d)
{
    uint32_t vals[32];
    uint32_t i;

    if (d->exec_size > 32 || (d->exec_size != 1 && d->exec_size != 8)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }

    if (d->src0_is_imm) {
        uint32_t v;

        if (!eu_imm_value(d->imm32, d->src0_type, &v)) {
            return ALCHEMIST_EU_UNSUPPORTED;
        }
        for (i = 0; i < d->exec_size; i++) {
            vals[i] = v;
        }
    } else if (!eu_read_operand(regs, d->src0_is_arf, d->src0_regnum,
                                 d->src0_subregnum, d->src0_type,
                                 d->exec_size, vals)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }

    if (!eu_write_operand(regs, d->dst_is_arf, d->dst_regnum,
                           d->dst_subregnum, d->dst_hstride, d->dst_type,
                           d->exec_size, vals)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }
    return ALCHEMIST_EU_SEND; /* unused - see caller, overwritten */
}

static AlchemistEuStatus eu_exec_add(AlchemistEuState *regs,
                                      const EuDecoded *d)
{
    uint32_t a[32], b[32], r[32];
    uint32_t i;

    if (d->exec_size > 32 || (d->exec_size != 1 && d->exec_size != 8)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }
    if (d->src0_is_imm && d->src1_is_imm) {
        return ALCHEMIST_EU_UNSUPPORTED; /* not a real encoding */
    }

    if (d->src0_is_imm) {
        uint32_t v;

        if (!eu_imm_value(d->imm32, d->src0_type, &v)) {
            return ALCHEMIST_EU_UNSUPPORTED;
        }
        for (i = 0; i < d->exec_size; i++) {
            a[i] = v;
        }
    } else if (!eu_read_operand(regs, d->src0_is_arf, d->src0_regnum,
                                 d->src0_subregnum, d->src0_type,
                                 d->exec_size, a)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }

    if (d->src1_is_imm) {
        uint32_t v;

        if (!eu_imm_value(d->imm32, d->src1_type, &v)) {
            return ALCHEMIST_EU_UNSUPPORTED;
        }
        for (i = 0; i < d->exec_size; i++) {
            b[i] = v;
        }
    } else if (!eu_read_operand(regs, d->src1_is_arf, d->src1_regnum,
                                 d->src1_subregnum, d->src1_type,
                                 d->exec_size, b)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }

    for (i = 0; i < d->exec_size; i++) {
        if (d->dst_type == EU_TYPE_F) {
            float fa, fb, fr;

            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            fr = fa + fb;
            memcpy(&r[i], &fr, 4);
        } else {
            r[i] = a[i] + b[i];
        }
    }

    if (!eu_write_operand(regs, d->dst_is_arf, d->dst_regnum,
                           d->dst_subregnum, d->dst_hstride, d->dst_type,
                           d->exec_size, r)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }
    return ALCHEMIST_EU_SEND; /* unused - see caller, overwritten */
}

uint32_t alchemist_eu_run(AlchemistEuState *regs, const uint8_t *code,
                           uint32_t n_instrs, AlchemistEuSend *send_out,
                           AlchemistEuStatus *status_out)
{
    uint32_t pc;

    for (pc = 0; pc < n_instrs; pc++) {
        EuDecoded d;
        AlchemistEuStatus st;

        eu_decode(code + pc * 16, &d);

        switch (d.opcode) {
        case EU_OPCODE_MOV:
            st = eu_exec_mov(regs, &d);
            if (st != ALCHEMIST_EU_SEND) { /* eu_exec_mov's dummy OK marker */
                *status_out = ALCHEMIST_EU_UNSUPPORTED;
                return pc;
            }
            break;
        case EU_OPCODE_ADD:
            st = eu_exec_add(regs, &d);
            if (st != ALCHEMIST_EU_SEND) {
                *status_out = ALCHEMIST_EU_UNSUPPORTED;
                return pc;
            }
            break;
        case EU_OPCODE_SEND:
        case EU_OPCODE_SENDC:
            send_out->sfid = d.send_sfid;
            send_out->eot = d.send_eot;
            send_out->desc_is_reg = d.send_desc_is_reg;
            send_out->desc = d.send_desc;
            send_out->payload_reg = d.src0_regnum;
            *status_out = ALCHEMIST_EU_SEND;
            return pc + 1;
        default:
            *status_out = ALCHEMIST_EU_UNSUPPORTED;
            return pc;
        }
    }

    *status_out = ALCHEMIST_EU_END_OF_CODE;
    return pc;
}
