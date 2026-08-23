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

#endif
