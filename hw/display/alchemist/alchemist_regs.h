/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - register definitions
 *
 * Offsets and bit values are transcribed directly from the real `xe`
 * driver source (torvalds/linux, drivers/gpu/drm/xe/regs/ and
 * xe_pcode_api.h etc.) so they can be diffed against the upstream
 * headers rather than guessed.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_DISPLAY_ALCHEMIST_REGS_H
#define HW_DISPLAY_ALCHEMIST_REGS_H

/* PCODE mailbox - drivers/gpu/drm/xe/xe_pcode_api.h */
#define ALCHEMIST_REG_PCODE_MAILBOX    0x138124
#define   PCODE_READY                  (1u << 31)
#define   PCODE_MB_COMMAND             0xFFu   /* bits [7:0] */
#define   PCODE_ERROR_MASK             0xFFu   /* bits [7:0], reply status */
#define     PCODE_SUCCESS              0x0u
#define ALCHEMIST_REG_PCODE_DATA0      0x138128
#define ALCHEMIST_REG_PCODE_DATA1      0x13812C

#define DGFX_PCODE_STATUS              0x7Eu
#define   DGFX_GET_INIT_STATUS         0x0u
#define   DGFX_INIT_STATUS_COMPLETE    0x1u

/*
 * Forcewake domains - drivers/gpu/drm/xe/regs/xe_gt_regs.h and
 * xe_force_wake.c. Every control/ack register pair uses the same
 * "masked write" convention: bits [31:16] of a write to the control
 * register select which of bits [15:0] are being updated (see
 * XE_REG_OPTION_MASKED in xe_reg_defs.h); the corresponding bit in the
 * ack register mirrors that state once the domain is awake. xe only
 * ever uses bit 0 (FORCEWAKE_KERNEL), but the masked-update logic here
 * handles the general case, not just bit 0.
 */
#define ALCHEMIST_REG_FORCEWAKE_GT         0xa188
#define ALCHEMIST_REG_FORCEWAKE_ACK_GT     0x130044
#define ALCHEMIST_REG_FORCEWAKE_RENDER     0xa278
#define ALCHEMIST_REG_FORCEWAKE_ACK_RENDER 0x000d84
#define ALCHEMIST_REG_FORCEWAKE_GSC        0xa618
#define ALCHEMIST_REG_FORCEWAKE_ACK_GSC    0x000df8
/* FORCEWAKE_MEDIA_VDBOX(n) / FORCEWAKE_ACK_MEDIA_VDBOX(n), n = 0..7 */
#define ALCHEMIST_REG_FORCEWAKE_MEDIA_VDBOX(n)     (0xa540 + (n) * 4)
#define ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VDBOX(n) (0x000d50 + (n) * 4)
/* FORCEWAKE_MEDIA_VEBOX(n) / FORCEWAKE_ACK_MEDIA_VEBOX(n), n = 0..3 */
#define ALCHEMIST_REG_FORCEWAKE_MEDIA_VEBOX(n)     (0xa560 + (n) * 4)
#define ALCHEMIST_REG_FORCEWAKE_ACK_MEDIA_VEBOX(n) (0x000d70 + (n) * 4)

/*
 * VRAM/tile sizing - drivers/gpu/drm/xe/regs/xe_regs.h and xe_gt_regs.h.
 * Both are plain (non-side-effecting) registers xe_vram_probe() reads
 * once during probe; see alchemist_vram.c for how their values are
 * derived from our real BAR2 size.
 */
#define ALCHEMIST_REG_SG_TILE_ADDR_RANGE(idx) (0x1083a0 + (idx) * 4)
#define   SG_TILE_SIZE_GB_SHIFT        8   /* GENMASK(17, 8), in GB units */
#define   SG_TILE_OFFSET_GB_SHIFT      1   /* GENMASK(7, 1), in GB units */

/* XEHP_FLAT_CCS_BASE_ADDR is an MCR register, but rw_with_mcr_steering()
 * always ends up reading/writing its raw offset regardless of steering -
 * see docs/alchemist-bringup.md for how this was confirmed. */
#define ALCHEMIST_REG_XEHP_FLAT_CCS_BASE_ADDR 0x4910
#define   XEHP_FLAT_CCS_PTR_SHIFT      8   /* GENMASK(31, 8), in 64K units */

/*
 * GuC firmware load - drivers/gpu/drm/xe/regs/xe_guc_regs.h and
 * xe_wopcm.c/xe_uc_fw.c/xe_guc.c. See alchemist_guc.c for the handshake
 * this implements.
 */
#define ALCHEMIST_REG_GUC_STATUS            0xc000
#define   GS_AUTH_STATUS_GOOD               (0x2u << 30)
#define   GS_UKERNEL_READY                  (0xF0u << 8)   /* XE_GUC_LOAD_STATUS_READY */
#define   GS_BOOTROM_JUMP_PASSED            (0x76u << 1)   /* XE_BOOTROM_STATUS_JUMP_PASSED */
#define   GS_MIA_IN_RESET                   (1u << 0)

#define ALCHEMIST_REG_GUC_WOPCM_SIZE        0xc050
#define   GUC_WOPCM_SIZE_LOCKED             (1u << 0)

#define ALCHEMIST_REG_DMA_CTRL              0xc314
#define   START_DMA                         (1u << 0)

#define ALCHEMIST_REG_DMA_GUC_WOPCM_OFFSET  0xc340
#define   GUC_WOPCM_OFFSET_VALID            (1u << 0)

/* GDRST - regs/xe_gt_regs.h. Reset-domain trigger register (do_gt_reset()/
 * xe_guc_reset() upstream write a domain bit and poll for hardware to
 * clear it once that domain's reset completes); we have no real
 * per-domain reset state to simulate, so clear immediately. xe_guc_reset()
 * writes GRDOM_GUC (0x8) specifically and then checks GUC_STATUS for
 * GS_MIA_IN_RESET - a real reset invalidates the whole prior boot state,
 * so we reset GUC_STATUS to just that bit rather than OR it in. */
#define ALCHEMIST_REG_GDRST                 0x941c
#define   GRDOM_GUC                         (1u << 3)

/*
 * GuC mmio mailbox (the "HXG" protocol) - abi/guc_messages_abi.h and
 * xe_guc.c's xe_guc_mmio_send_recv(). Distinct from the DMA/boot-status
 * registers above: this is how the host and GuC firmware exchange
 * request/response messages once GuC has booted. See alchemist_guc.c.
 */
/* xe_guc_notify() writes the main GT's "notify_reg", GUC_HOST_INTERRUPT -
 * any write at all (the value is always 0) rings the doorbell, it's not a
 * bit-flag register like GUC_SEND_INTERRUPT (which xe's own notify path
 * doesn't actually use, despite the similar name). */
#define ALCHEMIST_REG_GUC_HOST_INTERRUPT    0x1901f0

/* VF_SW_FLAG(n), n = 0..3 - request written here, response read back here. */
#define ALCHEMIST_REG_VF_SW_FLAG(n)         (0x190240 + (n) * 4)

#define HXG_MSG_0_ORIGIN_SHIFT              31
#define   HXG_ORIGIN_GUC                    1u
#define HXG_MSG_0_TYPE_SHIFT                28
#define   HXG_TYPE_MASK                     0x7u
#define   HXG_TYPE_REQUEST                  0u
#define   HXG_TYPE_EVENT                    1u
#define   HXG_TYPE_FAST_REQUEST             2u
#define   HXG_TYPE_RESPONSE_SUCCESS         7u
#define HXG_REQUEST_MSG_0_ACTION_MASK       0xFFFFu
#define HXG_RESPONSE_MSG_0_DATA0_MASK       0xFFFFFFFu   /* bits [27:0] */

/* XE_GUC_ACTION_GET_HWCONFIG - abi/guc_actions_abi.h */
#define GUC_ACTION_GET_HWCONFIG             0x4100u
/* GUC_ACTION_HOST2GUC_SELF_CFG - abi/guc_actions_abi.h. guc_self_cfg()
 * (xe_guc.c) treats a response data0 of exactly 1 as success (the count
 * of KLV entries configured) and 0 specifically as -ENOKEY. */
#define GUC_ACTION_HOST2GUC_SELF_CFG        0x0508u

/*
 * GGTT (Global GTT) - xe_mmio.c documents BAR0 as registers (0-4MB),
 * reserved (4-8MB), GGTT (8-16MB); xe_ggtt_init_early() confirms exactly
 * that with `ggtt->gsm = tile->mmio.regs + SZ_8M`. PTEs are plain 8-byte
 * MMIO stores at gsm[ggtt_addr >> XE_PTE_SHIFT] (xe_ggtt_set_pte()), so
 * they already land correctly in our generic buffer - alchemist_ggtt.c
 * only needs to decode them. Format is regs/xe_gtt_defs.h: bit 0 present,
 * bit 1 XE_GGTT_PTE_DM (1 = address is a VRAM/BAR2 offset, 0 = a system
 * RAM physical address), bits [51:12] the (4K-page-shifted) address - the
 * 8MB GSM region sized for exactly 4GiB of GGTT space at 4K/PTE confirms
 * the shift is 12, not e.g. a 64K-page variant.
 */
#define ALCHEMIST_GGTT_GSM_BASE      0x800000u   /* 8MB offset in BAR0 */
#define ALCHEMIST_GGTT_PAGE_SHIFT    12
#define ALCHEMIST_GGTT_PAGE_SIZE     (1u << ALCHEMIST_GGTT_PAGE_SHIFT)
#define   XE_PAGE_PRESENT            (1ull << 0)
#define   XE_GGTT_PTE_DM             (1ull << 1)
#define   XE_PAGE_ADDR_MASK          0x000FFFFFFFFFF000ull  /* bits [51:12] */

/*
 * CTB (Command Transport Buffer) - abi/guc_communication_ctb_abi.h and
 * abi/guc_klvs_abi.h. The guest registers descriptor/ring GGTT addresses
 * for both directions via SELF_CFG (see alchemist_guc.c); this is the
 * real ring-buffer protocol those addresses are for. See alchemist_ctb.c.
 */
#define GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY             0x0902u
#define GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY  0x0903u
#define GUC_KLV_SELF_CFG_H2G_CTB_SIZE_KEY             0x0904u
#define GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY             0x0905u
#define GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY  0x0906u
#define GUC_KLV_SELF_CFG_G2H_CTB_SIZE_KEY             0x0907u

/* struct guc_ct_buffer_desc - 64 bytes, but only the first 3 dwords are
 * used: head/tail are ring offsets in DWORDS, updated head only by the
 * receiver and tail only by the sender - so for H2G we own head and the
 * guest owns tail; for G2H it's reversed. */
#define CTB_DESC_OFF_HEAD    0
#define CTB_DESC_OFF_TAIL    4
#define CTB_DESC_OFF_STATUS  8

#define CTB_MSG_0_FENCE_SHIFT       16
#define CTB_MSG_0_FORMAT_SHIFT      12
#define   CTB_FORMAT_HXG            0u
#define CTB_MSG_0_NUM_DWORDS_MASK   0xFFu

/*
 * GT interrupt cascade - regs/xe_irq_regs.h. Real Intel GPUs route
 * interrupts through several indirection levels (master enable -> a
 * per-bank status word -> a select-then-read "identity" lookup for the
 * specific source) rather than a flat status register. We only need to
 * support one source so far - GuC2Host, bank 0 bit INTR_GUC - since we
 * have no engines/submission implemented yet to raise anything else, but
 * the mechanism itself (see alchemist_irq.c) is implemented for real:
 * the driver's own dg1_irq_handler()/gt_engine_identity() walk exactly
 * this cascade regardless of how many sources exist behind it.
 */
#define ALCHEMIST_REG_DG1_MSTR_TILE_INTR    0x190008
#define   DG1_MSTR_IRQ                       (1u << 31)
#define   DG1_MSTR_TILE0                     (1u << 0)

#define ALCHEMIST_REG_GFX_MSTR_IRQ           0x190010
#define   MASTER_IRQ                         (1u << 31)
#define   GT_DW_IRQ0                         (1u << 0)

#define ALCHEMIST_REG_GT_INTR_DW(bank)       (0x190018 + (bank) * 4)
#define   INTR_GUC                           (1u << 25)

#define ALCHEMIST_REG_IIR_REG_SELECTOR(bank) (0x190070 + (bank) * 4)
#define ALCHEMIST_REG_INTR_IDENTITY_REG(bank) (0x190060 + (bank) * 4)
#define   INTR_DATA_VALID                    (1u << 31)
#define   INTR_ENGINE_INSTANCE_SHIFT         20
#define   INTR_ENGINE_CLASS_SHIFT            16
#define   XE_ENGINE_CLASS_OTHER              4u
#define   OTHER_GUC_INSTANCE                 0u

/* GUC_INTR_GUC2HOST - regs/xe_guc_regs.h */
#define   GUC_INTR_GUC2HOST                  (1u << 15)

/*
 * RCS0 (render engine) completion interrupt - same bank-0 GT_INTR_DW
 * cascade as GuC2Host above, a different bit/identity. XE_ENGINE_CLASS_RENDER
 * and GT_MI_USER_INTERRUPT - xe_hw_engine_types.h / regs/xe_irq_regs.h.
 */
#define   INTR_RCS0                          (1u << 0)
#define   XE_ENGINE_CLASS_RENDER             0u
#define   GT_MI_USER_INTERRUPT               (1u << 0)

/*
 * GuC context registration/scheduling actions - abi/guc_actions_abi.h.
 * Payloads transcribed from xe_guc_submit.c (__register_exec_queue(),
 * submit_exec_queue(), MAKE_SCHED_CONTEXT_ACTION()) - see alchemist_submit.c.
 */
#define GUC_ACTION_REGISTER_CONTEXT              0x4502u
#define XE_GUC_ACTION_SCHED_CONTEXT               0x1000u
#define XE_GUC_ACTION_SCHED_CONTEXT_MODE_SET      0x1001u
#define XE_GUC_ACTION_SCHED_CONTEXT_MODE_DONE     0x1002u
#define   GUC_CONTEXT_DISABLE                     0u
#define   GUC_CONTEXT_ENABLE                      1u

/*
 * LRC (Logical Ring Context) layout - regs/xe_lrc_layout.h. The engine
 * register-state image starts LRC_PPHWSP_SIZE past the LRC's own GGTT
 * address (which is the PPHWSP address, xe_lrc_ggtt_addr()); ring
 * head/tail/start/ctl are fixed dword offsets within that image.
 */
#define LRC_PPHWSP_SIZE                     0x1000u
#define CTX_RING_HEAD_OFF                   0x14u   /* dword (0x04+1) */
#define CTX_RING_TAIL_OFF                   0x1Cu   /* dword (0x06+1) */
#define CTX_RING_START_OFF                  0x24u   /* dword (0x08+1) */
#define CTX_RING_CTL_OFF                    0x2Cu   /* dword (0x0a+1) */
/* regs/xe_engine_regs.h RING_HEAD/RING_TAIL - HEAD is 4-byte, TAIL 8-byte
 * aligned; both are byte offsets into the ring, not dword indices. */
#define RING_HEAD_ADDR_MASK                 0x1FFFFCu
#define RING_TAIL_ADDR_MASK                 0x1FFFF8u
#define RING_CTL_VALID                      1u

/*
 * xe_lrc_descriptor()'s low bits (LRC_VALID/ADDRESSING_MODE/PRIVILEGE) -
 * xe_lrc.c - mask off to recover the plain page-aligned PPHWSP GGTT
 * address the REGISTER_CONTEXT hwlrca_lo/hi fields carry.
 */
#define LRC_DESC_ADDR_MASK                  0xFFFFFFFFFFFFF000ULL

/*
 * The literal MI-instruction ring epilogue every submitted job ends with
 * (xe_ring_ops.c __emit_job_gen12_render_compute(): emit_pipe_imm_ggtt()
 * + emit_user_interrupt()) - see alchemist_submit.c. Recognizing this
 * fixed 9-dword tail, rather than interpreting the whole ring/batch, is
 * enough to signal completion correctly.
 */
#define MI_EPILOGUE_DWORDS                  9u
/* xe_lrc_write_ring() QWORD-aligns the ring tail with an MI_NOOP pad
 * when needed (9 dwords isn't a multiple of 2) - tolerate that. */
#define MI_EPILOGUE_PAD_TOLERANCE           2u
#define GFX_OP_PIPE_CONTROL_LEN6            0x7A000004u
#define PIPE_CONTROL_BREADCRUMB_FLAGS \
    (0x01000000u /* GLOBAL_GTT_IVB */ | 0x00004000u /* QW_WRITE */ | \
     0x00000080u /* FLUSH_ENABLE */   | 0x00100000u /* CS_STALL */)
#define MI_USER_INTERRUPT                   0x01000000u
#define MI_ARB_ON_OFF_ENABLE                0x04000001u
#define MI_ARB_OFF                          0x04000000u
#define MI_ARB_CHECK                        0x02800000u

/*
 * The "simple" ring epilogue (xe_ring_ops.c __emit_job_gen12_simple(),
 * used by XE_ENGINE_CLASS_COPY and others with no EU/aux handling) - an
 * MI_FLUSH_DW breadcrumb instead of render/compute's PIPE_CONTROL, 8
 * dwords not 9 (already QWORD-aligned, no pad dword expected, but
 * alchemist_submit.c still searches for MI_ARB_CHECK rather than
 * assuming a fixed offset, same as the render/compute epilogue).
 */
#define MI_SIMPLE_EPILOGUE_DWORDS           8u
#define MI_FLUSH_DW_STOREDW_IMM             0x13004002u
#define MI_FLUSH_DW_USE_GTT_BIT             0x00000004u
#define XE_ENGINE_CLASS_COPY                3u
#define INTR_BCS0                           (1u << 15)

#endif
