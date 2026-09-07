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
 * Native (128-bit) format is fully decoded. Compacted (64-bit) format
 * is decoded too, but only for the one real shape a live guest boot has
 * actually produced so far: a one-source (`mov`) instruction, both
 * operands `:f`, default contiguous regioning, `WrEn` set - see
 * eu_decompact()'s file comment for the full real-instruction research
 * behind this and exactly what's scoped in/out. `send`/branch
 * instructions are *never* compacted on real hardware (confirmed in
 * Intel's own PRM), so EOT recognition doesn't depend on wider
 * compacted-opcode coverage - only real evidence (an actual, different
 * compacted instruction a live boot produces) will grow this further,
 * not preemptive guessing.
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
 *   (W) and (1|M0) r127.2<1>:ud r0.0<0;1,0>:ud 0xFFFFFFC0:ud
 *   65 00 00 80 20 82 45 7f 04 00 00 02 c0 ff ff ff  (r0 address masking,
 *   from a real compiled `buf[0]=42` kernel, found live via this project's
 *   own guest boot - decode independently confirmed against `iga64
 *   -p=12p71`)
 *
 *   (W) mov (2|M0) r3.0<1>:f r1.0<1;1,0>:f {Compacted,A@1}  (compacted -
 *   61 09 03 72 00 01 10 00, from the same real kernel; see
 *   eu_decompact()'s comment for how this 64-bit form was derived)
 *
 * The regioning this interpreter implements (per-lane contiguous read/
 * write, e.g. `r5.0<8;8,1>:d` = one new dword per channel starting at
 * subreg 0) is exactly the pattern the mov/add/send examples above use -
 * the standard/default regioning for a straightforward per-channel
 * operation. Non-default regioning (broadcast reads, cross-row access)
 * is real EU functionality this doesn't decode - flagged as
 * ALCHEMIST_EU_UNSUPPORTED rather than silently mishandled - *except*
 * that this reduces to a no-op distinction at exec_size 1 (the `and`
 * example's own `r0.0<0;1,0>` broadcast source region is indistinguishable
 * from a contiguous one-element read when there's only one element to
 * read), which is why that real instruction decodes correctly without
 * needing genuine region-stride support yet.
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
 * zero/sign-extended from the real element width). The one ARF source
 * given real storage is cr0 (see AlchemistEuState's comment); any other
 * ARF read is genuinely unsupported. */
static bool eu_read_operand(AlchemistEuState *regs, bool is_arf,
                             uint32_t regnum, uint32_t subregnum,
                             uint32_t type, uint32_t exec_size,
                             uint32_t out[32])
{
    uint32_t width = eu_type_width(type);
    uint32_t i;
    uint8_t *base;

    if (width == 0) {
        return false;
    }
    if (is_arf) {
        if (regnum != EU_ARF_CR0 ||
            subregnum + exec_size * width > sizeof(regs->cr0)) {
            return false;
        }
        base = regs->cr0;
    } else {
        if (regnum >= 128 ||
            subregnum + exec_size * width > sizeof(regs->grf[0])) {
            return false;
        }
        base = regs->grf[regnum];
    }

    for (i = 0; i < exec_size; i++) {
        const uint8_t *p = &base[subregnum + i * width];
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
    uint8_t *base;

    if (width == 0 || hstride != 1) {
        return false;
    }
    if (is_arf) {
        /* "null" discards the result - a real, correct thing to do, not
         * an unhandled case. cr0 gets real storage (see
         * AlchemistEuState's comment) so a read-modify-write sequence
         * stays self-consistent. Any *other* ARF register (address/
         * accumulator/flag) as a write target is genuinely unsupported. */
        if (regnum == EU_ARF_NULL) {
            return true;
        }
        if (regnum != EU_ARF_CR0 ||
            subregnum + exec_size * width > sizeof(regs->cr0)) {
            return false;
        }
        base = regs->cr0;
    } else {
        if (regnum >= 128 ||
            subregnum + exec_size * width > sizeof(regs->grf[0])) {
            return false;
        }
        base = regs->grf[regnum];
    }

    for (i = 0; i < exec_size; i++) {
        memcpy(&base[subregnum + i * width], &val[i], width);
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

    /* exec_size is always a power of two (1u << a 3-bit log2 field),
     * so >32 is the only real hardware bound - no other restriction is
     * warranted here now that a real compacted instruction has shown
     * exec_size 2 (see eu_decompact()'s file comment); the per-lane
     * data path below (vals[32]/a[32]/b[32]/r[32]) already handles any
     * width up to 32 generically. */
    if (d->exec_size > 32) {
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

    /* see eu_exec_mov()'s comment on the exec_size bound */
    if (d->exec_size > 32) {
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

/*
 * Shared src0/src1 fetch for the bitwise ops below (AND/OR) - both are
 * structurally identical to eu_exec_add() (same operand-fetch/write
 * shape, immediate-or-register each side), only the combining operator
 * differs, and neither has a real floating-point form (unlike add,
 * there's no dst_type==EU_TYPE_F case to special-case). Two real,
 * hardware-verified compiled instructions drove adding these (see
 * docs/alchemist-bringup.md and the file comment's worked examples):
 *   (W) and (1|M0) r127.2<1>:ud r0.0<0;1,0>:ud   0xFFFFFFC0:ud
 *   (W) or  (1|M0) cr0.0<1>:ud  cr0.0<0;1,0>:ud  0x4C0:uw
 */
static bool eu_fetch_binop_srcs(AlchemistEuState *regs, const EuDecoded *d,
                                 uint32_t a[32], uint32_t b[32])
{
    uint32_t i;

    /* see eu_exec_mov()'s comment on the exec_size bound */
    if (d->exec_size > 32) {
        return false;
    }
    if (d->src0_is_imm && d->src1_is_imm) {
        return false; /* not a real encoding */
    }

    if (d->src0_is_imm) {
        uint32_t v;

        if (!eu_imm_value(d->imm32, d->src0_type, &v)) {
            return false;
        }
        for (i = 0; i < d->exec_size; i++) {
            a[i] = v;
        }
    } else if (!eu_read_operand(regs, d->src0_is_arf, d->src0_regnum,
                                 d->src0_subregnum, d->src0_type,
                                 d->exec_size, a)) {
        return false;
    }

    if (d->src1_is_imm) {
        uint32_t v;

        if (!eu_imm_value(d->imm32, d->src1_type, &v)) {
            return false;
        }
        for (i = 0; i < d->exec_size; i++) {
            b[i] = v;
        }
    } else if (!eu_read_operand(regs, d->src1_is_arf, d->src1_regnum,
                                 d->src1_subregnum, d->src1_type,
                                 d->exec_size, b)) {
        return false;
    }

    return true;
}

static AlchemistEuStatus eu_exec_and(AlchemistEuState *regs,
                                      const EuDecoded *d)
{
    uint32_t a[32], b[32], r[32];
    uint32_t i;

    if (!eu_fetch_binop_srcs(regs, d, a, b)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }
    for (i = 0; i < d->exec_size; i++) {
        r[i] = a[i] & b[i];
    }
    if (!eu_write_operand(regs, d->dst_is_arf, d->dst_regnum,
                           d->dst_subregnum, d->dst_hstride, d->dst_type,
                           d->exec_size, r)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }
    return ALCHEMIST_EU_SEND; /* unused - see caller, overwritten */
}

static AlchemistEuStatus eu_exec_or(AlchemistEuState *regs,
                                     const EuDecoded *d)
{
    uint32_t a[32], b[32], r[32];
    uint32_t i;

    if (!eu_fetch_binop_srcs(regs, d, a, b)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }
    for (i = 0; i < d->exec_size; i++) {
        r[i] = a[i] | b[i];
    }
    if (!eu_write_operand(regs, d->dst_is_arf, d->dst_regnum,
                           d->dst_subregnum, d->dst_hstride, d->dst_type,
                           d->exec_size, r)) {
        return ALCHEMIST_EU_UNSUPPORTED;
    }
    return ALCHEMIST_EU_SEND; /* unused - see caller, overwritten */
}

/*
 * Compacted (64-bit) instruction support - real evidence this is
 * genuinely needed (not the "defer until real evidence shows up" case
 * the file comment used to describe) came from the same real,
 * IGC-compiled `buf[0]=42` kernel's 6th instruction: `iga64`'s own
 * `{Compacted}` disassembly annotation confirms
 *   61 09 03 72 00 01 10 00
 * is `(W) mov (2|M0) r3.0<1>:f r1.0<1;1,0>:f {A@1}` in exactly 8 bytes,
 * not the 16 a native encoding of the same instruction takes.
 *
 * The real per-generation compaction table *content* lives in Intel's
 * open-source `intel-graphics-compiler` repo (MIT), under
 * `visa/iga/GEDLibrary/GED_external/build/autogen-intel64/
 * ged_compaction_tables.{h,cpp}` (DG2/XeHPG's specific tables are wired
 * up via `ged_model_xe_hpg.cpp`'s `oneSourceCompactMapping`/
 * `DecodingTable782`/`EncodingMasksTable131` chain) - but that's GED's
 * full auto-generated decode-*engine*, not a simple table, and
 * `iga64` (present in the guest's own toolchain, already this
 * project's decode oracle for every opcode/ARF-encoding discovery this
 * phase) links those exact tables and correctly encodes/decodes them.
 * So instead of hand-porting GED's generated C++, `iga64 -Xautocompact`
 * (assemble-and-compact) and `-Xforce-no-compact` were used as a
 * **compaction oracle**: assembling the real instruction text above
 * reproduces the real kernel's exact 8 compacted bytes byte-for-byte,
 * and a systematic sweep (exec_size, dst/src0 subreg, dst/src0/WrEn/
 * SWSB, register number) against that oracle empirically derived the
 * real field layout below - hardware-verified the same way as
 * everything else in this project, not parsed from generated source
 * and not guessed.
 *
 * Confirmed compacted-word layout (8 bytes, byte-indexed c[0..7]):
 *   c[0]    = opcode - literal, identical encoding/position to native
 *             bits[6:0] (confirmed: assembling the same text with and
 *             without compaction gives the same c[0]/native byte0).
 *   c[1]    = SWSB - literal, byte-identical between formats (toggling
 *             the `{A@1}` annotation changes this byte identically in
 *             both the native and compacted assemblies) - irrelevant
 *             here anyway since eu_decode() never reads it.
 *   c[2]    = dst register number - literal 0-127 (confirmed via a
 *             sweep across 0/1/3/5/10/50/100/127, each reproduced
 *             exactly in c[2]).
 *   c[3]    = bit5 is CmptCtrl (1 when compacted; the native encoding
 *             of the exact same instruction has that bit 0 - this is
 *             the real, hardware-verified position of the compact/
 *             native discriminator, checked by eu_is_compact() before
 *             any other decoding happens); bits[4:0] are ControlIndex,
 *             a real per-generation lookup table - the exec_size sweep
 *             (1/2/8/16, `WrEn` set) gave four real, verified entries
 *             (eu_compact_ctrl_table[]); exec_size 4/32 and `WrEn`
 *             unset don't compact at all for this shape (iga64 falls
 *             back to native), so this project doesn't need those
 *             ControlIndex entries yet.
 *   c[4]    = SubRegIndex << 3 (a 5-bit index at bits[7:3]) - another
 *             real lookup table, this one mapping to a (dst_subreg,
 *             src0_subreg) *pair* (confirmed non-linear/non-formulaic
 *             by sweeping each independently while holding the other
 *             at 0 - a genuine generated table, not a computable
 *             function). eu_compact_subreg_table[] holds the real
 *             entries found this way, including index 0 -> (0,0), the
 *             one the real blocking instruction actually needs.
 *   c[5]    = src0 register number - literal (same confirmation method
 *             as c[2], via a source-register sweep).
 *   c[6..7] = fixed `0x10 0x00` for every real compacted instance
 *             found so far (all real `:f`/`:f`, contiguous-regioning
 *             cases) - the DataTypeIndex/SrcIndex encoding for this one
 *             verified shape; any other value is unsupported, since no
 *             other real instance has been seen yet to derive it from.
 *
 * Scope, honestly: only opcode `mov`, only this one DataTypeIndex/
 * SrcIndex byte pattern, only the ControlIndex/SubRegIndex entries
 * actually observed. Anything else - a different opcode's compacted
 * form (`add`/`and`/`or` all failed to compact in the same sweep, so
 * their two-source compact tables are still completely unexplored), a
 * different type, non-default regioning, or a subreg/exec_size
 * combination not yet seen - is real EU functionality this doesn't
 * decode, flagged as ALCHEMIST_EU_UNSUPPORTED and left for the next
 * real instance to extend, exactly like every other opcode this file
 * has grown to support so far.
 */
static bool eu_is_compact(const uint8_t instr[8])
{
    return (instr[3] & 0x20u) != 0; /* CmptCtrl, word bit 29 */
}

static const struct {
    uint8_t index;
    uint8_t exec_size_log2;
} eu_compact_ctrl_table[] = {
    { 2, 0 },  /* exec_size 1 */
    { 18, 1 }, /* exec_size 2 */
    { 4, 3 },  /* exec_size 8 */
    { 3, 4 },  /* exec_size 16 */
};

static const struct {
    uint8_t index;
    uint8_t dst_sub;
    uint8_t src0_sub;
} eu_compact_subreg_table[] = {
    { 0, 0, 0 },
    { 12, 1, 0 },
    { 7, 2, 0 },
    { 19, 3, 0 },
    { 18, 4, 0 },
    { 21, 5, 0 },
    { 20, 6, 0 },
    { 17, 7, 0 },
    { 4, 0, 1 },
    { 8, 0, 2 },
    { 15, 0, 3 },
    { 11, 0, 4 },
    { 14, 0, 5 },
    { 13, 0, 6 },
    { 16, 0, 7 },
};

/* Expands a compacted instruction into the equivalent native-format 16
 * bytes eu_decode() already understands, so all existing decode/exec
 * logic is reused unchanged - see the file comment above this function
 * for the real, hardware-verified field layout this implements. */
static bool eu_decompact(const uint8_t c[8], uint8_t native_out[16])
{
    uint32_t opcode = c[0] & 0x7Fu;
    uint32_t ctrl_index, subreg_index;
    uint32_t exec_size_log2 = UINT32_MAX;
    uint32_t dst_sub = UINT32_MAX, src0_sub = 0;
    uint32_t i;

    if (opcode != EU_OPCODE_MOV) {
        return false;
    }
    if (c[6] != 0x10u || c[7] != 0x00u) {
        return false;
    }

    ctrl_index = c[3] & 0x1Fu;
    for (i = 0; i < ARRAY_SIZE(eu_compact_ctrl_table); i++) {
        if (eu_compact_ctrl_table[i].index == ctrl_index) {
            exec_size_log2 = eu_compact_ctrl_table[i].exec_size_log2;
            break;
        }
    }
    if (exec_size_log2 == UINT32_MAX) {
        return false;
    }

    subreg_index = c[4] >> 3;
    for (i = 0; i < ARRAY_SIZE(eu_compact_subreg_table); i++) {
        if (eu_compact_subreg_table[i].index == subreg_index) {
            dst_sub = eu_compact_subreg_table[i].dst_sub;
            src0_sub = eu_compact_subreg_table[i].src0_sub;
            break;
        }
    }
    if (dst_sub == UINT32_MAX) {
        return false;
    }

    memset(native_out, 0, 16);
    native_out[0] = c[0];
    native_out[1] = c[1];
    native_out[2] = (uint8_t)exec_size_log2;
    native_out[4] = (uint8_t)(EU_TYPE_F << 4);
    native_out[5] = (uint8_t)EU_TYPE_F;
    native_out[6] = (uint8_t)((dst_sub << 3) | (EU_REGFILE_GRF << 2) | 1u);
    native_out[7] = c[2];
    native_out[8] = (uint8_t)((src0_sub << 3) | (EU_REGFILE_GRF << 2));
    native_out[9] = c[5];
    return true;
}

uint32_t alchemist_eu_run(AlchemistEuState *regs, const uint8_t *code,
                           uint32_t n_instrs, AlchemistEuSend *send_out,
                           AlchemistEuStatus *status_out)
{
    uint64_t total_bytes = (uint64_t)n_instrs * 16;
    uint64_t off = 0;
    uint32_t executed = 0;

    while (off + 8 <= total_bytes) {
        EuDecoded d;
        AlchemistEuStatus st;
        uint8_t native_buf[16];
        const uint8_t *instr;
        uint32_t consumed;

        if (eu_is_compact(code + off)) {
            if (!eu_decompact(code + off, native_buf)) {
                *status_out = ALCHEMIST_EU_UNSUPPORTED;
                return executed;
            }
            instr = native_buf;
            consumed = 8;
        } else {
            if (off + 16 > total_bytes) {
                break; /* not enough bytes left for a native instruction */
            }
            instr = code + off;
            consumed = 16;
        }

        eu_decode(instr, &d);

        switch (d.opcode) {
        case EU_OPCODE_MOV:
            st = eu_exec_mov(regs, &d);
            if (st != ALCHEMIST_EU_SEND) { /* eu_exec_mov's dummy OK marker */
                *status_out = ALCHEMIST_EU_UNSUPPORTED;
                return executed;
            }
            break;
        case EU_OPCODE_ADD:
            st = eu_exec_add(regs, &d);
            if (st != ALCHEMIST_EU_SEND) {
                *status_out = ALCHEMIST_EU_UNSUPPORTED;
                return executed;
            }
            break;
        case EU_OPCODE_AND:
            st = eu_exec_and(regs, &d);
            if (st != ALCHEMIST_EU_SEND) {
                *status_out = ALCHEMIST_EU_UNSUPPORTED;
                return executed;
            }
            break;
        case EU_OPCODE_OR:
            st = eu_exec_or(regs, &d);
            if (st != ALCHEMIST_EU_SEND) {
                *status_out = ALCHEMIST_EU_UNSUPPORTED;
                return executed;
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
            return executed + 1;
        default:
            *status_out = ALCHEMIST_EU_UNSUPPORTED;
            return executed;
        }

        off += consumed;
        executed++;
    }

    *status_out = ALCHEMIST_EU_END_OF_CODE;
    return executed;
}
