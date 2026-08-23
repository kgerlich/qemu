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

#endif
