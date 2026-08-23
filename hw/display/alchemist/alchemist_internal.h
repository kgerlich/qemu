/*
 * Intel Arc "Alchemist" (DG2) GPU simulation - shared internal state
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_DISPLAY_ALCHEMIST_INTERNAL_H
#define HW_DISPLAY_ALCHEMIST_INTERNAL_H

#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"

typedef struct AlchemistState {
    PCIDevice pdev;
    MemoryRegion mmio;
    MemoryRegion vram;
    uint8_t *mmio_buf;
} AlchemistState;

static inline uint32_t alchemist_mmio_load32(AlchemistState *s, hwaddr addr)
{
    uint32_t val;

    memcpy(&val, s->mmio_buf + addr, sizeof(val));
    return val;
}

static inline void alchemist_mmio_store32(AlchemistState *s, hwaddr addr,
                                           uint32_t val)
{
    memcpy(s->mmio_buf + addr, &val, sizeof(val));
}

/*
 * Phase-specific write hooks. Each is called after the generic buffer
 * store has already happened (so DATA/param registers written just
 * before a "go" register are already visible), and handles side effects
 * for its own register(s) only - everything else stays plain
 * read/write-what-was-written memory.
 */
void alchemist_pcode_mmio_write(AlchemistState *s, hwaddr addr, unsigned size);
void alchemist_forcewake_mmio_write(AlchemistState *s, hwaddr addr, unsigned size);

/*
 * Called once from realize() to pre-populate registers that just report a
 * fixed value rather than reacting to guest writes.
 */
void alchemist_vram_init(AlchemistState *s);

#endif
