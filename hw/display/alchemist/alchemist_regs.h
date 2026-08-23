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

#define ALCHEMIST_REG_GUC_WOPCM_SIZE        0xc050
#define   GUC_WOPCM_SIZE_LOCKED             (1u << 0)

#define ALCHEMIST_REG_DMA_CTRL              0xc314
#define   START_DMA                         (1u << 0)

#define ALCHEMIST_REG_DMA_GUC_WOPCM_OFFSET  0xc340
#define   GUC_WOPCM_OFFSET_VALID            (1u << 0)

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

#endif
