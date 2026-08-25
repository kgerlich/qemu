/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - VRAM/tile sizing registers
 *
 * xe_vram_probe() (xe_vram.c upstream) derives usable VRAM size from two
 * plain, non-side-effecting registers rather than from the LMEM BAR size
 * directly:
 *
 *   - SG_TILE_ADDR_RANGE(0): tile size and offset, in 1GB units.
 *   - XEHP_FLAT_CCS_BASE_ADDR: offset (in 64K units) marking where flat-CCS
 *     compression metadata storage begins; everything below it is usable
 *     VRAM. tile_vram_size() computes usable_size = ccs_offset - tile_offset.
 *
 * We report a tile size of exactly 1GB (the smallest value the 1GB-
 * granularity SG_TILE_ADDR_RANGE field can represent) with the flat-CCS
 * base set to the same 1GB offset, i.e. no space reserved for CCS
 * metadata - all of the (fictional) 1GB tile is usable.
 *
 * Our real BAR2 (GMADR/LMEM aperture) is smaller than that at 256MB, so
 * vram_region_init() clamps usable size down to what's actually mapped
 * and logs "Small BAR device" - this is DG2's real, documented behavior
 * on hardware without Resizable BAR enabled (see the kernel's
 * Documentation/gpu/rfc/i915_small_bar.rst), not a shortcut we invented.
 *
 * Also sets FUSE2's PRODUCTION_HW bit - xe_device_verify_dontcopy_wa()
 * reads it to distinguish production from pre-production silicon;
 * leaving it clear makes the driver log an error and taint the kernel.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

void alchemist_vram_init(AlchemistState *s)
{
    uint64_t tile_size_gb = 1;
    uint32_t tile_addr_range = (tile_size_gb << SG_TILE_SIZE_GB_SHIFT);
    uint64_t flat_ccs_base_64k = (tile_size_gb * GiB) / (64 * KiB);
    uint32_t flat_ccs_base_addr = (uint32_t)(flat_ccs_base_64k << XEHP_FLAT_CCS_PTR_SHIFT);

    alchemist_mmio_store32(s, ALCHEMIST_REG_SG_TILE_ADDR_RANGE(0), tile_addr_range);
    alchemist_mmio_store32(s, ALCHEMIST_REG_XEHP_FLAT_CCS_BASE_ADDR, flat_ccs_base_addr);
    alchemist_mmio_store32(s, ALCHEMIST_REG_FUSE2, PRODUCTION_HW);
}
