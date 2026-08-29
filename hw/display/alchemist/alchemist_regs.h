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

/* FUSE2 - regs/xe_gt_regs.h. xe_device_verify_dontcopy_wa() (xe_device.c)
 * reads bit PRODUCTION_HW to distinguish production from pre-production
 * silicon; leaving it unset makes the driver log an error and taint the
 * kernel, which "clean dmesg" doesn't allow for a device claiming to be
 * a real, shipped DG2 card. */
#define ALCHEMIST_REG_FUSE2            0x9120
#define   PRODUCTION_HW                (1u << 2)

/*
 * GT topology fuse registers - regs/xe_gt_regs.h, read by
 * xe_gt_topology_init() (xe_gt_topology.c) to build the DSS (dual
 * subslice)/EU masks compute-runtime later queries via
 * DRM_XE_DEVICE_QUERY_GT_TOPOLOGY. DG2 (graphics_xehpg descriptor,
 * xe_pci.c) uses exactly one fuse register for each of geometry/compute
 * (num_geometry_xecore_fuse_regs = num_compute_xecore_fuse_regs = 1), so
 * dss_per_quad = 32*1/4 = 8 (xe_gt_topology_has_dss_in_quadrant()) -
 * reporting DSS 0 present in the compute mask keeps quadrant 0 (and so
 * CCS0) from being fused off. Leaving these at their generic-buffer
 * default of 0 (no DSS/EU anywhere) is what caused compute-runtime's
 * IoctlHelperXe::createEngineInfo() to find zero compute-class engines
 * and UNRECOVERABLE_IF(!defaultEngine) - confirmed directly against a
 * real `intel-opencl-icd` abort, see docs/alchemist-bringup.md.
 */
#define ALCHEMIST_REG_XELP_EU_ENABLE               0x9134
#define   XELP_EU_MASK                             0xFFu   /* bits [7:0] */
#define ALCHEMIST_REG_XELP_GT_GEOMETRY_DSS_ENABLE  0x913c
#define ALCHEMIST_REG_XEHP_GT_COMPUTE_DSS_ENABLE   0x9144

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

/*
 * GuC hwconfig table content - a flat sequence of [key, len_dw, value...]
 * KLV entries (NEO's shared/source/os_interface/linux/system_info.cpp
 * DeviceBlobConstants - the real userspace-side parser for this exact
 * table, confirmed by decoding a real compute-runtime SIGFPE: with no
 * real entries (the deliberate deferral this used to be - see
 * alchemist_guc.c's file comment), NEO::SystemInfo::parseDeviceBlob()
 * leaves maxSlicesSupported at its default-constructed 0, and
 * IoctlHelperXe::getTopologyDataAndMap() unconditionally divides by it
 * (MaxSubSlicesSupported / MaxSlicesSupported) with no zero-check - a
 * real NEO bug that only surfaces once a real compute-capable device
 * (CCS0 not fused off) is actually queried this deeply, which nothing
 * before Phase 13 ever exercised.
 *
 * Values reported here are not invented - they're the exact same
 * topology already established via the GT fuse registers above
 * (alchemist_vram_init(): 8 active DSS, EU_ENABLE=0xFF -> 16 EU/DSS per
 * xe_hw_engine.c's SIMD8-doubling, DG2::setupHardwareInfoBase()'s own
 * hardcoded NumThreadsPerEu=8u), reported through this second real
 * protocol path GuC hwconfig represents - the same real configuration,
 * not a second, independently-fabricated one.
 */
#define HWCONFIG_KEY_MAX_SLICES_SUPPORTED       1u
#define HWCONFIG_KEY_MAX_DUAL_SUBSLICES_SUPPORTED 2u
#define HWCONFIG_KEY_MAX_EU_PER_DUAL_SUBSLICE   3u
#define HWCONFIG_KEY_NUM_THREADS_PER_EU         15u
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

/* CCS0 (compute engine) completion interrupt - same bank-0 GT_INTR_DW
 * cascade, INTR_CCS(x) = REG_BIT(4+x) - regs/xe_irq_regs.h. Compute
 * dispatches use the exact same ring_ops/epilogue as render
 * (xe_ring_ops.c: XE_ENGINE_CLASS_COMPUTE and _RENDER both resolve to
 * emit_job_gen12_render_compute()) - only the interrupt identity
 * differs. */
#define   INTR_CCS0                          (1u << 4)
/* NOT xe's internal enum xe_engine_class (xe_hw_engine_types.h), which
 * has XE_ENGINE_CLASS_COMPUTE = 5 - the REGISTER_CONTEXT payload's
 * engine_class field actually carries GuC's OWN class enum
 * (abi/guc_scheduler_abi.h's GUC_RENDER_CLASS=0/GUC_VIDEO_CLASS=1/
 * GUC_VIDEOENHANCE_CLASS=2/GUC_BLITTER_CLASS=3/GUC_COMPUTE_CLASS=4 -
 * translated from xe's internal enum by xe_engine_class_to_guc_class(),
 * xe_guc_ads.c), a DIFFERENT numbering that only happens to coincide
 * with xe's own for RENDER(0) and COPY/BLITTER(3) - confirmed the hard
 * way: a real hwe ccs0 workaround job's REGISTER_CONTEXT arrived with
 * engine_class=4, not 5, causing it to fall through this project's
 * switch's `default: return` (silently, correctly-by-design ignoring an
 * unrecognized class) and time out. See docs/alchemist-bringup.md. */
#define   XE_ENGINE_CLASS_COMPUTE            4u

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
 * The "simple" ring epilogue (xe_ring_ops.c, XE_ENGINE_CLASS_COPY) - an
 * MI_FLUSH_DW breadcrumb instead of render/compute's PIPE_CONTROL.
 * __emit_job_gen12_simple() itself (a plain job with no VM_BIND/migrate
 * content) is a fixed [MI_ARB_OFF, MI_FLUSH_DW(4dw), suffix(3dw)] = 8
 * dwords, but xe_migrate.c's migrate/VM_BIND-update jobs
 * (emit_migration_job_gen12(), also XE_ENGINE_CLASS_COPY) are a much
 * richer, real, distinct shape: two MI_BATCH_BUFFER_START jumps, an
 * intermediate "start seqno" MI_FLUSH_DW breadcrumb wrapped in
 * preparser_disable(true/false), and only THEN the real completion
 * MI_FLUSH_DW - so MI_ARB_OFF is no longer immediately adjacent to the
 * completion write. Confirmed directly against a real
 * `clCreateBuffer(CL_MEM_COPY_HOST_PTR)`-triggered migrate job that our
 * previous fixed 8-dword/ARB_OFF-anchored window silently failed to
 * recognize, stalling job completion (`docs/alchemist-bringup.md`).
 * Since only the *last* MI_FLUSH_DW + the fixed 3-dword suffix is ever
 * needed for completion (the intermediate "start seqno" write, any
 * batch content, and MI_ARB_OFF's exact position are all irrelevant to
 * *that*), dropping the leading MI_ARB_OFF dword from the window - 7
 * dwords, not 8 - handles both the plain and migrate job shapes
 * uniformly, without special-casing either.
 *
 * MI_FLUSH_DW's header also carries real, independently-varying
 * optional flags (instructions/xe_mi_commands.h) - migrate jobs pass
 * `job->migrate_flush_flags` through (MI_FLUSH_DW_CCS and/or
 * MI_INVALIDATE_TLB, set or clear depending on what the specific job
 * touches - confirmed live with both bits clear, MI_FLUSH_DW_CCS alone,
 * and both set together), which the plain job's fixed
 * emit_flush_imm_ggtt() call never sets - so only these two flag bits,
 * actually confirmed varying, are masked out of the comparison;
 * anything else still has to match exactly, not silently tolerated.
 */
#define MI_SIMPLE_EPILOGUE_DWORDS           7u
#define MI_FLUSH_DW_STOREDW_IMM             0x13004002u
#define MI_FLUSH_DW_CCS                     0x00010000u  /* REG_BIT(16) */
#define MI_INVALIDATE_TLB                   0x00040000u  /* REG_BIT(18) */
#define MI_FLUSH_DW_USE_GTT_BIT             0x00000004u
#define XE_ENGINE_CLASS_COPY                3u
#define INTR_BCS0                           (1u << 15)

/*
 * GuC PC (SLPC - Single Loop Power Control), abi/guc_actions_slpc_abi.h.
 * A CTB HXG_TYPE_REQUEST action (xe_guc_pc.c pc_action_reset() etc.) -
 * see alchemist_pc.c. Unlike every other REQUEST so far, success isn't
 * just the CTB ack: xe_guc_pc_start() polls a driver-allocated,
 * GGTT-mapped shared buffer's global_state field directly (not the CT
 * response), which real GuC firmware writes as a side effect of
 * processing SLPC_EVENT_RESET.
 */
#define GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST 0x3003u
#define HOST2GUC_PC_SLPC_REQUEST_EVENT_ID_SHIFT  8u
#define HOST2GUC_PC_SLPC_REQUEST_EVENT_ID_MASK   0xFFu
#define   SLPC_EVENT_RESET                  0u
/* struct slpc_shared_data_header - abi/guc_actions_slpc_abi.h: size(u32)
 * then global_state(u32) at byte offset 4. */
#define SLPC_SHARED_DATA_GLOBAL_STATE_OFF   4u
#define   SLPC_GLOBAL_STATE_RUNNING         3u

/*
 * TLB invalidation - abi/guc_actions_abi.h, xe_guc_tlb_inval.c
 * (send_tlb_inval_ggtt()/send_tlb_inval_ppgtt() etc.). A fire-and-forget
 * H2G (no synchronous CTB reply - sent via plain xe_guc_ct_send(), not
 * the _recv() variant that would promote it to HXG_TYPE_REQUEST, same
 * "no g2h_fence" reasoning as GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST above)
 * whose real completion is a separate, unsolicited G2H event
 * (XE_GUC_ACTION_TLB_INVALIDATION_DONE, 1-dword payload: the same seqno
 * the H2G action carried) - xe_guc_tlb_inval_done_handler() asserts
 * seqnos arrive in order and wakes any waiters/fences for it. Since this
 * project never caches a translation (every PPGTT/GGTT read/write walks
 * real, current guest memory fresh - alchemist_ggtt.c/alchemist_ppgtt.c),
 * "invalidation" is vacuously already true the instant it's requested;
 * acknowledging it immediately is the real, correct completion, not a
 * shortcut - confirmed necessary live (a real compute-runtime buffer
 * bind stalls waiting for this ack, "TLB invalidation fence timeout"),
 * see docs/alchemist-bringup.md.
 */
#define XE_GUC_ACTION_TLB_INVALIDATION       0x7000u
#define XE_GUC_ACTION_TLB_INVALIDATION_DONE  0x7001u
#define XE_GUC_ACTION_TLB_INVALIDATION_ALL   0x7002u

/*
 * PPGTT (per-process page tables) - regs/xe_gtt_defs.h, xe_pt.c, xe_vm.c.
 * A real 4-level radix tree (DG2: xe->info.vm_max_level == 3, i.e.
 * levels 0-3; xe_pt.c's xe_normal_pt_shifts[] = {12,21,30,39,48}), 9-bit
 * index per level, 4KB page-table nodes, 8-byte entries - see
 * alchemist_ppgtt.c. Bit positions genuinely differ from GGTT's (e.g.
 * the VRAM/local-memory indicator is bit 11 here, not GGTT's bit 1) -
 * do not reuse the XE_GGTT_PTE_* macros above for PPGTT decode.
 *
 * Root address is a per-context LRC field, CTX_PDP0_UDW/_LDW
 * (regs/xe_lrc_layout.h dword indices 0x31/0x33), written once at
 * context/LRC init the same way CTX_RING_START etc. already are.
 */
#define CTX_PDP0_UDW_OFF                    0xC4u   /* dword (0x30+1) */
#define CTX_PDP0_LDW_OFF                    0xCCu   /* dword (0x32+1) */

#define XE_PPGTT_MAX_LEVEL                  3u      /* DG2 vm_max_level */
#define XE_PPGTT_PAGE_TABLE_ENTRIES         512u    /* 4KB / 8 bytes */
#define XE_PPGTT_LEVEL_SHIFT(level)         (12u + 9u * (level))

#define XE_PAGE_RW                          (1ull << 1)
#define XE_PDE_64K                          (1ull << 6)   /* level-1 PDE: child table is compact (64K-leaf) */
#define XE_PDE_PS_2M                        (1ull << 7)   /* level-1 PDE: this entry is a 2MB leaf, not a pointer */
#define XE_PDPE_PS_1G                       (1ull << 7)   /* level-2 PDE: this entry is a 1GB leaf, not a pointer */
#define XE_PTE_PS64                         (1ull << 8)   /* level-0 PTE: hint only, doesn't affect address decode */
#define XE_PPGTT_PTE_DM                     (1ull << 11)  /* leaf: address is a VRAM/BAR2 offset */

/* xe_compact_pt_shifts[] - the level-0 (leaf) shift becomes 16 instead
 * of 12 whenever the level-1 PDE pointing at that leaf table had
 * XE_PDE_64K set (each leaf entry then covers 64K, table covers 32MB). */
#define XE_PPGTT_COMPACT_LEAF_SHIFT         16u

/*
 * Resizable BAR (PCIe ECN "Resizable BAR Capability" / regs/xe_bars.h's
 * LMEM_BAR=2) - xe_pci_rebar.c's xe_pci_rebar_resize() actively calls the
 * kernel's pci_resize_resource() at probe if BAR2 is smaller than the
 * capability's advertised max, and real DG2 hardware always exposes this
 * capability (not just when BIOS pre-sizes the BAR large). We report the
 * one size we actually back (ALCHEMIST_VRAM_SIZE, see alchemist_internal.h)
 * as both the only supported size and the current size, so the driver's
 * own resize call sees current==max and no-ops - a real, correct
 * configuration (matching a BIOS-preconfigured-large-BAR system), not a
 * partial implementation pretending to support live resize.
 *
 * Standard ECN encoding (not xe-specific): a size field value V means
 * 1MB << V; CAP's bit (4+V) advertises support for that size. 1GB = V=10.
 */
#define ALCHEMIST_REBAR_BAR_INDEX      2u
#define ALCHEMIST_REBAR_SIZE_ENCODING  10u  /* 1MB << 10 == 1GB */
#define ALCHEMIST_REBAR_CAP_VAL        (1u << (4u + ALCHEMIST_REBAR_SIZE_ENCODING))
#define ALCHEMIST_REBAR_CTRL_VAL \
    (ALCHEMIST_REBAR_BAR_INDEX |                    /* PCI_REBAR_CTRL_BAR_IDX */ \
     (1u << 5) |                                     /* PCI_REBAR_CTRL_NBAR_MASK: 1 resizable BAR */ \
     (ALCHEMIST_REBAR_SIZE_ENCODING << 8))            /* PCI_REBAR_CTRL_BAR_SIZE */

/*
 * EU (execution unit) native 128-bit instruction format - Gen12.5/DG2.
 * Bit positions cross-confirmed from Mesa's src/intel/compiler/gen/xe.json,
 * Intel's IGA (GED library) decode tables, and independently hardware-
 * verified by hand-decoding real ocloc/iga64-compiled DG2 bytes field-by-
 * field (see alchemist_eu.c and docs/alchemist-bringup.md) - every field
 * below matched real compiled output exactly, not derived from a single
 * source alone. Compact (64-bit) instructions are NOT decoded (a real,
 * deliberate gap - see alchemist_eu.c's file comment); send/branch
 * instructions are never compacted on real hardware either, so EOT
 * recognition is unaffected by this.
 */
#define EU_OPCODE_MOV     0x61u  /* Gen12+ only - opcode 1 on Gen9-11 */
#define EU_OPCODE_ADD     0x40u
#define EU_OPCODE_AND     0x65u  /* hardware-verified via iga64 -p=12p71
                                   * against real ocloc-compiled `buf[0]=42`
                                   * kernel bytes - see docs/alchemist-bringup.md */
#define EU_OPCODE_OR      0x66u  /* same verification as EU_OPCODE_AND */
#define EU_OPCODE_SEND    0x31u
#define EU_OPCODE_SENDC   0x32u

/* Confirmed type-index encodings (hardware-decoded, not just table-listed) */
#define EU_TYPE_UB  0x0u
#define EU_TYPE_UW  0x1u
#define EU_TYPE_UD  0x2u
#define EU_TYPE_W   0x5u
#define EU_TYPE_D   0x6u
#define EU_TYPE_F   0xAu

#define EU_REGFILE_ARF 0u
#define EU_REGFILE_GRF 1u
#define EU_ARF_NULL    0u
#define EU_ARF_CR0     0x80u /* Control Register 0 - hardware-verified via
                               * iga64 -p=12p71 (`cr0.0` decodes to ARF
                               * regnum 0x80) */

#define EU_SFID_MESSAGE_GATEWAY 0x3u
#define EU_SFID_UGM             0xFu

/*
 * LSC (Load/Store/Control-message) descriptor bit layout - Mesa's
 * gen_encoding.cpp gen_lsc_desc_decode()/gen_helpers.h, hardware-verified
 * against a real ocloc-compiled `buf[0] = 42` kernel's send.ugm bytes
 * (desc == 0x020E8584, see alchemist_gpgpu.c and docs/alchemist-bringup.md).
 * Only the fields needed to recognize a flat/stateless 64-bit-address,
 * 32-bit-scalar store (this phase's whole scope - anything else is left
 * alone, not guessed at) are named here.
 */
#define LSC_DESC_OP_MASK             0x3Fu        /* bits [5:0] */
#define   LSC_OP_STORE               0x4u
#define LSC_DESC_ADDR_SIZE_SHIFT     7u            /* bits [8:7] */
#define LSC_DESC_ADDR_SIZE_MASK      0x3u
#define   LSC_ADDR_SIZE_A64          0x3u
#define LSC_DESC_DATA_SIZE_SHIFT     9u            /* bits [11:9] */
#define LSC_DESC_DATA_SIZE_MASK      0x7u
#define   LSC_DATA_SIZE_D32          0x2u
#define LSC_DESC_VECT_SIZE_SHIFT     12u           /* bits [14:12] */
#define LSC_DESC_VECT_SIZE_MASK      0x7u
#define   LSC_VECT_SIZE_V1           0x0u
#define LSC_DESC_MSG_LENGTH_SHIFT    25u           /* bits [28:25], src0/address GRFs */
#define LSC_DESC_MSG_LENGTH_MASK     0xFu
#define LSC_DESC_ADDR_TYPE_SHIFT     29u           /* bits [30:29] */
#define LSC_DESC_ADDR_TYPE_MASK      0x3u
#define   LSC_ADDR_SURFTYPE_FLAT     0x0u

/*
 * GPGPU/compute command stream - gen125.xml, cross-confirmed against
 * real ocloc-compiled command sequences (see alchemist_gpgpu.c). Command
 * type field, bits[31:29] of any instruction/command header dword.
 */
#define GPGPU_CMD_TYPE_MI       0u
#define GPGPU_CMD_TYPE_GFXPIPE  3u

/* PIPELINE_SELECT - gen125.xml: Type=3,SubType=1,Opcode=1,SubOpcode=4
 * (bits[31:16]); always exactly 1 dword, no length field at all. */
#define PIPELINE_SELECT_HDR_MASK   0xFFFF0000u
#define PIPELINE_SELECT_HDR_VALUE  0x69040000u

/* STATE_BASE_ADDRESS - gen125.xml: Type=3,SubType=0,Opcode=1,SubOpcode=1
 * (bits[31:16]). Instruction Base Address is a 2-dword address field
 * (dwords 10-11 relative to the command start, bits[63:12]); bit 0 of
 * dword 10 is its own "Modify Enable" flag. */
#define STATE_BASE_ADDRESS_HDR_MASK        0xFFFF0000u
#define STATE_BASE_ADDRESS_HDR_VALUE       0x61010000u
#define STATE_BASE_ADDRESS_INSTR_BASE_DW   10u
#define   STATE_BASE_ADDRESS_INSTR_BASE_MODIFY_EN (1u << 0)

/* CFE_STATE / COMPUTE_WALKER - gen125.xml: Type=3,Pipeline=2(GPGPU),
 * ComputeCmdOpcode=2, CFE-SubOpcode at bits[23:18] (0=CFE_STATE,
 * 2=COMPUTE_WALKER) - bits[31:18] together identify which one; bits
 * [17:16] (SubOpcode Variant) are deliberately excluded from the match
 * mask since they're real, independently-varying fields, not part of
 * the command's identity. */
#define GPGPU_CMD_HDR_MASK       0xFFFC0000u
#define CFE_STATE_HDR_VALUE      0x72000000u
#define COMPUTE_WALKER_HDR_VALUE 0x72080000u

/* COMPUTE_WALKER - gen125.xml COMPUTE_WALKER_BODY (39 dwords total,
 * dwords 0-38; the body struct's own field numbering starts at body-
 * relative dword 1 = absolute dword 2 - all indices below are already
 * absolute). Only the fields a minimal 1x1x1/1-thread dispatch with an
 * inline-only cross-thread payload needs are named here. */
#define COMPUTE_WALKER_DWORDS         39u
#define CW_DW_INDIRECT_DATA_LENGTH    2u
#define CW_DW_INDIRECT_DATA_START     3u
#define CW_DW_EXEC_CONTROL            4u
#define   CW_EMIT_INLINE_PARAMETER    (1u << 25)
#define CW_DW_GROUP_DIM_X             7u
#define CW_DW_GROUP_DIM_Y             8u
#define CW_DW_GROUP_DIM_Z             9u
#define CW_DW_IDD_KERNEL_START        18u  /* bits[31:6] */
#define   CW_KERNEL_START_MASK        0xFFFFFFC0u
#define CW_DW_IDD_THREADS_IN_GROUP    23u  /* bits[9:0] */
#define   CW_THREADS_IN_GROUP_MASK    0x3FFu
#define CW_DW_INLINE_DATA_START       31u  /* 8 dwords (32 bytes), through 38 */

/* MI_BATCH_BUFFER_START - xe_mi_commands.h __MI_INSTR(0x31) - matched by
 * opcode only (bits[31:23]); low bits (ppgtt flag, length, ...) vary
 * legitimately per use and are decoded separately. */
#define MI_OPCODE_HDR_MASK              0xFF800000u
#define MI_BATCH_BUFFER_START_HDR_VALUE 0x18800000u
#define MI_BATCH_BUFFER_START_PPGTT_FLAG (1u << 8)
#define MI_BATCH_BUFFER_END_HDR_VALUE   0x05000000u

/* Pure malformed-input guards for alchemist_gpgpu.c's forward walkers -
 * not a real hardware limit, just a bound on how far we'll walk looking
 * for MI_BATCH_BUFFER_START (in the ring) or COMPUTE_WALKER/
 * MI_BATCH_BUFFER_END (in the indirect batch) before giving up on
 * content that doesn't look like a real, well-formed command stream. A
 * real minimal OpenCL dispatch batch is on the order of tens of dwords
 * (gen125.xml command lengths above), so a few hundred is generous. */
#define GPGPU_RING_WALK_GUARD  256u
#define GPGPU_BATCH_WALK_GUARD 512u

/* Real compute-runtime command buffers nest a second MI_BATCH_BUFFER_START
 * inside the first (a setup/flush PIPE_CONTROL, then a jump into the
 * "real" command buffer carrying PIPELINE_SELECT/STATE_BASE_ADDRESS/
 * CFE_STATE/COMPUTE_WALKER) - confirmed live against a real dispatch, see
 * docs/alchemist-bringup.md. gpgpu_process_batch() follows nested jumps
 * recursively; this bounds recursion depth against malformed/adversarial
 * content chaining jumps indefinitely - real command buffers never nest
 * more than one or two levels deep. */
#define GPGPU_MAX_BATCH_NESTING 4u

/* Bound on how many EU instructions alchemist_gpgpu.c will fetch from a
 * dispatched kernel before giving up looking for a send/EOT - a real
 * trivial OpenCL kernel (mov + send{EOT}, per Phase 12's research) is 2
 * instructions; generous enough for a real but still-simple kernel
 * without buffering an unbounded amount of guest-controlled "code". */
#define GPGPU_MAX_KERNEL_INSTRS 64u

#endif
