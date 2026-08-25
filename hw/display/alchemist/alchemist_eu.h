/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - EU (execution unit)
 * instruction-set interpreter types
 *
 * See alchemist_eu.c for the full design rationale. This header is
 * separate from alchemist_internal.h because these types are specific
 * to EU program execution (consumed by whichever later phase dispatches
 * compute/render work to a simulated thread - not yet wired to anything
 * as of this phase), not core device state.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_DISPLAY_ALCHEMIST_EU_H
#define HW_DISPLAY_ALCHEMIST_EU_H

/* 128 GRF registers x 32 bytes each - real Gen12.5 EU thread register
 * file size (xe_guc_ads.c/PRM). Kept flat/byte-addressed since real
 * hardware sub-register addressing is itself byte-granular. */
typedef struct AlchemistEuState {
    uint8_t grf[128][32];
} AlchemistEuState;

/* A decoded `send`/`sendc` envelope - see alchemist_eu.c's file comment
 * for exactly which fields are (and aren't) decoded. Filled in by
 * alchemist_eu_run()/alchemist_eu_step() when execution stops on a send;
 * a later phase acts on it (dispatching by sfid/desc) and is responsible
 * for any actual message side effects - this file only decodes the
 * envelope, it never performs memory operations itself. */
typedef struct AlchemistEuSend {
    uint32_t sfid;
    bool eot;
    bool desc_is_reg;
    uint32_t desc;
    uint32_t payload_reg; /* src0 GRF regnum - the message payload base */
} AlchemistEuSend;

typedef enum AlchemistEuStatus {
    ALCHEMIST_EU_SEND,        /* stopped on a send/sendc - see *send_out */
    ALCHEMIST_EU_UNSUPPORTED, /* stopped: decoded something outside this
                                * phase's scope (see alchemist_eu.c) -
                                * never silently guessed at */
    ALCHEMIST_EU_END_OF_CODE, /* ran off the end of the supplied buffer
                                * without hitting a send - malformed
                                * input, not a real program */
} AlchemistEuStatus;

/*
 * Executes native (128-bit, uncompacted) EU instructions from `code`
 * (n_instrs * 16 bytes) against `regs`, starting at instruction 0, until
 * a send/sendc is decoded or something unsupported is hit. Returns the
 * number of instructions actually executed before stopping; *status_out
 * and (for ALCHEMIST_EU_SEND) *send_out describe why.
 */
uint32_t alchemist_eu_run(AlchemistEuState *regs, const uint8_t *code,
                           uint32_t n_instrs, AlchemistEuSend *send_out,
                           AlchemistEuStatus *status_out);

#endif
