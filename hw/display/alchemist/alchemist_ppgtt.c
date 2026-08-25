/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - PPGTT (per-process page
 * tables) address translation
 *
 * Unlike GGTT (one flat table), PPGTT is a real radix tree - confirmed
 * from xe_vm.c/xe_pt.c/regs/xe_gtt_defs.h: on DG2 (xe->info.vm_max_level
 * == 3) it's 4 levels (0-3), 9-bit index per level, 4KB page-table
 * nodes, 8-byte entries (xe_pt.c's xe_normal_pt_shifts[] =
 * {12,21,30,39,48}). Level 0 is always a leaf; levels 1/2 are directory
 * pointers unless their PS bit (XE_PDE_PS_2M/XE_PDPE_PS_1G, both bit 7)
 * reinterprets the slot as a 2MB/1GB huge leaf; level 3 (the root) is
 * always a pointer. A level-1 PDE additionally has XE_PDE_64K (bit 6),
 * which switches its *child* (level-0) table to "compact" mode - 64K
 * leaves instead of 4K, matching DG2's requirement that VRAM allocations
 * land on 64K boundaries (XE_VRAM_FLAGS_NEED64K).
 *
 * Root address discovery, real completion protocol, and VRAM/system-RAM
 * dispatch are all covered by alchemist_ggtt.c's existing machinery and
 * reused here rather than duplicated:
 *
 * - VM_BIND (xe_vm.c/xe_pt.c/xe_migrate.c, confirmed via
 *   xe_migrate_update_pgtables_cpu()) is 100% CPU-side for a fresh,
 *   non-rebind bind - the driver just writes PTE qwords directly into
 *   memory we already expose. No register or GuC-message hook exists to
 *   miss; this file is a pure read-side decode, same as GGTT.
 * - The root page table's GGTT address is a per-context LRC field,
 *   CTX_PDP0_UDW/_LDW (regs/xe_lrc_layout.h), written once at context
 *   init the same way CTX_RING_START etc. already are - resolved here
 *   through the guc_id -> LRC tracking alchemist_submit.c already
 *   maintains for command submission.
 * - Every page-table node (root, directory, or leaf) is itself
 *   GGTT-resident memory (xe_pt_create() allocates a plain 4KB xe_bo per
 *   node), so walking the tree is just repeated alchemist_ggtt_read()
 *   calls - no separate VRAM/system-RAM dispatch logic needed here, GGTT
 *   already resolved that once for the node fetch itself.
 *
 * PAT/cacheability bits are deliberately not modeled (they don't affect
 * where an address decodes to, only caching behavior, which this project
 * doesn't simulate) - see XE_PAGE_ADDR_MASK's comment in alchemist_regs.h
 * for the one place this leaves a genuinely open, flagged assumption
 * (whether PDE_PDPE_PAT2, bit 12, can ever collide with a directory
 * pointer's own address bit 12 on real DG2 hardware).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

static bool ppgtt_get_root(AlchemistState *s, uint32_t guc_id,
                            uint64_t *root_addr)
{
    uint64_t regs_off;
    uint32_t udw, ldw;

    if (guc_id >= ALCHEMIST_MAX_CONTEXTS || !s->ctx[guc_id].registered) {
        return false;
    }

    regs_off = s->ctx[guc_id].lrc_ggtt_addr + LRC_PPHWSP_SIZE;

    if (!alchemist_ggtt_read(s, regs_off + CTX_PDP0_UDW_OFF, &udw, 4) ||
        !alchemist_ggtt_read(s, regs_off + CTX_PDP0_LDW_OFF, &ldw, 4)) {
        return false;
    }

    *root_addr = (((uint64_t)udw << 32) | ldw) & XE_PAGE_ADDR_MASK;
    return true;
}

/*
 * Walks the tree for a single gpu_va, returning the target address (a
 * VRAM/BAR2 offset if *is_vram, else a guest system-physical address)
 * and the size of the leaf page it was found in - callers need the real
 * leaf size (4K/64K/2M/1G) to chunk a multi-page transfer correctly,
 * unlike GGTT's fixed 4K pages.
 */
static bool ppgtt_translate_page(AlchemistState *s, uint64_t root_addr,
                                  uint64_t gpu_va, uint64_t *phys,
                                  bool *is_vram, uint64_t *page_size)
{
    uint64_t node_addr = root_addr;
    bool compact = false;
    int level;
    uint32_t shift, index;
    uint64_t entry;

    for (level = XE_PPGTT_MAX_LEVEL; level >= 1; level--) {
        shift = XE_PPGTT_LEVEL_SHIFT(level);
        index = (gpu_va >> shift) & (XE_PPGTT_PAGE_TABLE_ENTRIES - 1);

        if (!alchemist_ggtt_read(s, node_addr + (uint64_t)index * 8,
                                  &entry, 8)) {
            return false;
        }
        if (!(entry & XE_PAGE_PRESENT)) {
            return false;
        }

        if ((level == 2 && (entry & XE_PDPE_PS_1G)) ||
            (level == 1 && (entry & XE_PDE_PS_2M))) {
            *page_size = 1ull << shift;
            *phys = (entry & XE_PAGE_ADDR_MASK) | (gpu_va & (*page_size - 1));
            *is_vram = !!(entry & XE_PPGTT_PTE_DM);
            return true;
        }

        node_addr = entry & XE_PAGE_ADDR_MASK;
        if (level == 1 && (entry & XE_PDE_64K)) {
            compact = true;
        }
    }

    /* level 0: always a leaf. */
    shift = compact ? XE_PPGTT_COMPACT_LEAF_SHIFT : XE_PPGTT_LEVEL_SHIFT(0);
    index = (gpu_va >> shift) & (XE_PPGTT_PAGE_TABLE_ENTRIES - 1);

    if (!alchemist_ggtt_read(s, node_addr + (uint64_t)index * 8, &entry, 8)) {
        return false;
    }
    if (!(entry & XE_PAGE_PRESENT)) {
        return false;
    }

    *page_size = 1ull << shift;
    *phys = (entry & XE_PAGE_ADDR_MASK) | (gpu_va & (*page_size - 1));
    *is_vram = !!(entry & XE_PPGTT_PTE_DM);
    return true;
}

/* Both read and write walk the range one leaf page at a time - PPGTT
 * pages aren't even a fixed size (4K/64K/2M/1G), let alone guaranteed
 * physically contiguous across consecutive VAs, so each chunk needs its
 * own fresh translation - same reasoning as alchemist_ggtt_read/write. */
bool alchemist_ppgtt_read(AlchemistState *s, uint32_t guc_id,
                           uint64_t gpu_va, void *buf, uint64_t len)
{
    uint8_t *dst = buf;
    uint64_t root_addr;

    if (!ppgtt_get_root(s, guc_id, &root_addr)) {
        return false;
    }

    while (len > 0) {
        uint64_t phys, page_size, page_off, chunk;
        bool is_vram;

        if (!ppgtt_translate_page(s, root_addr, gpu_va, &phys, &is_vram,
                                   &page_size)) {
            return false;
        }

        page_off = gpu_va & (page_size - 1);
        chunk = MIN(len, page_size - page_off);

        if (is_vram) {
            if (phys + chunk > ALCHEMIST_VRAM_SIZE) {
                return false;
            }
            memcpy(dst, s->vram_ptr + phys, chunk);
        } else {
            if (pci_dma_read(&s->pdev, phys, dst, chunk) != MEMTX_OK) {
                return false;
            }
        }

        dst += chunk;
        gpu_va += chunk;
        len -= chunk;
    }

    return true;
}

bool alchemist_ppgtt_write(AlchemistState *s, uint32_t guc_id,
                            uint64_t gpu_va, const void *buf, uint64_t len)
{
    const uint8_t *src = buf;
    uint64_t root_addr;

    if (!ppgtt_get_root(s, guc_id, &root_addr)) {
        return false;
    }

    while (len > 0) {
        uint64_t phys, page_size, page_off, chunk;
        bool is_vram;

        if (!ppgtt_translate_page(s, root_addr, gpu_va, &phys, &is_vram,
                                   &page_size)) {
            return false;
        }

        page_off = gpu_va & (page_size - 1);
        chunk = MIN(len, page_size - page_off);

        if (is_vram) {
            if (phys + chunk > ALCHEMIST_VRAM_SIZE) {
                return false;
            }
            memcpy(s->vram_ptr + phys, src, chunk);
        } else {
            if (pci_dma_write(&s->pdev, phys, src, chunk) != MEMTX_OK) {
                return false;
            }
        }

        src += chunk;
        gpu_va += chunk;
        len -= chunk;
    }

    return true;
}
