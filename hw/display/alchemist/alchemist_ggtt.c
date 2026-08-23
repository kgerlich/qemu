/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - GGTT address translation
 *
 * The GGTT (Global GTT) lets the guest hand us a GPU-visible address and
 * have it resolve to either a system-RAM physical address or an offset
 * into our own VRAM (BAR2), exactly as it would on real hardware - this
 * is how the guest tells us where its CTB descriptors/rings and other
 * GuC-visible buffers live, and it's how we actually reach into guest
 * memory to service them.
 *
 * PTEs live at a fixed, well-known offset within BAR0 (see
 * alchemist_regs.h for exactly how that offset and the PTE bit layout
 * were confirmed against xe_mmio.c/xe_ggtt.c/xe_gtt_defs.h) and are
 * written there as plain 8-byte MMIO stores by the guest's own
 * xe_ggtt_set_pte() - our generic buffer already stores them correctly,
 * so translation is a pure read-side decode, no write-side hook needed.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "alchemist_internal.h"
#include "alchemist_regs.h"

/*
 * Decode the PTE covering ggtt_addr. Returns false if the PTE offset falls
 * outside BAR0 (an out-of-range GGTT address) or the PTE isn't marked
 * present (nothing mapped there) - either way, nothing can be transferred.
 * On success, *phys is the target address (a VRAM/BAR2 offset if
 * *is_vram, otherwise a guest system-physical address) covering
 * ggtt_addr specifically, not just its containing page.
 */
static bool alchemist_ggtt_translate_page(AlchemistState *s, uint64_t ggtt_addr,
                                           uint64_t *phys, bool *is_vram)
{
    uint64_t pte_index = ggtt_addr >> ALCHEMIST_GGTT_PAGE_SHIFT;
    uint64_t pte_offset = ALCHEMIST_GGTT_GSM_BASE + pte_index * 8;
    uint64_t pte;
    uint64_t page_base, page_off;

    if (pte_offset + 8 > ALCHEMIST_MMIO_SIZE) {
        return false;
    }

    memcpy(&pte, s->mmio_buf + pte_offset, sizeof(pte));

    if (!(pte & XE_PAGE_PRESENT)) {
        return false;
    }

    page_base = pte & XE_PAGE_ADDR_MASK;
    page_off = ggtt_addr & (ALCHEMIST_GGTT_PAGE_SIZE - 1);

    *phys = page_base | page_off;
    *is_vram = !!(pte & XE_GGTT_PTE_DM);
    return true;
}

/*
 * Both read and write walk the range one page at a time, since
 * consecutive GGTT addresses are under no obligation to map to
 * contiguous physical pages - a multi-page transfer that assumed
 * contiguity after translating only the first page would silently
 * corrupt data on any guest allocation that happens not to be physically
 * contiguous.
 */
bool alchemist_ggtt_read(AlchemistState *s, uint64_t ggtt_addr, void *buf,
                          uint64_t len)
{
    uint8_t *dst = buf;

    while (len > 0) {
        uint64_t phys;
        bool is_vram;
        uint64_t page_off = ggtt_addr & (ALCHEMIST_GGTT_PAGE_SIZE - 1);
        uint64_t chunk = MIN(len, ALCHEMIST_GGTT_PAGE_SIZE - page_off);

        if (!alchemist_ggtt_translate_page(s, ggtt_addr, &phys, &is_vram)) {
            return false;
        }

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
        ggtt_addr += chunk;
        len -= chunk;
    }

    return true;
}

bool alchemist_ggtt_write(AlchemistState *s, uint64_t ggtt_addr,
                           const void *buf, uint64_t len)
{
    const uint8_t *src = buf;

    while (len > 0) {
        uint64_t phys;
        bool is_vram;
        uint64_t page_off = ggtt_addr & (ALCHEMIST_GGTT_PAGE_SIZE - 1);
        uint64_t chunk = MIN(len, ALCHEMIST_GGTT_PAGE_SIZE - page_off);

        if (!alchemist_ggtt_translate_page(s, ggtt_addr, &phys, &is_vram)) {
            return false;
        }

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
        ggtt_addr += chunk;
        len -= chunk;
    }

    return true;
}
