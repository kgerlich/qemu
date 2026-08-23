/*
 * Intel Arc "Alchemist" (DG2, Xe-HPG, Gen12.5) GPU simulation
 *
 * A QEMU device model aiming to present enough of a real DG2's PCI/MMIO
 * surface that the unmodified upstream Linux `xe` driver believes it is
 * talking to real hardware. See docs/alchemist-bringup.md for the bring-up
 * log and the project plan for phase-by-phase scope.
 *
 * This file holds the PCI device core: identity, BAR setup, and the
 * top-level MMIO dispatcher. Phase-specific register behavior (PCODE
 * mailbox, forcewake, GuC firmware handshake, ...) lives in sibling files
 * and is wired into alchemist_mmio_write() as each phase lands.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"
#include "qemu/module.h"
#include "alchemist_internal.h"

#define TYPE_PCI_ALCHEMIST_DEVICE "alchemist"
DECLARE_INSTANCE_CHECKER(AlchemistState, ALCHEMIST,
                          TYPE_PCI_ALCHEMIST_DEVICE)

/*
 * BAR0 (GTTMMADR): combined MMIO register space + Global GTT. The real xe
 * driver's xe_mmio_probe_early() rejects anything smaller than 16MB before
 * reading a single register, so this is a hard floor, not a tuning knob.
 */
#define ALCHEMIST_MMIO_SIZE (16 * MiB)

/*
 * BAR2 (GMADR): local-memory/VRAM aperture. Real DG2 cards expose several
 * GB here; we start intentionally small and revisit if a later phase's
 * VRAM-size probe rejects it.
 */
#define ALCHEMIST_VRAM_SIZE (256 * MiB)

/* DG2-G11 (Arc A380 desktop), the smallest die - see docs/alchemist-bringup.md */
#define ALCHEMIST_PCI_DEVICE_ID 0x56a5
#define ALCHEMIST_PCI_REVISION  0x08

static uint64_t alchemist_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    AlchemistState *s = opaque;
    uint64_t val = 0;

    memcpy(&val, s->mmio_buf + addr, size);
    return val;
}

static void alchemist_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    AlchemistState *s = opaque;

    memcpy(s->mmio_buf + addr, &val, size);

    alchemist_pcode_mmio_write(s, addr, size);
    alchemist_forcewake_mmio_write(s, addr, size);
}

static const MemoryRegionOps alchemist_mmio_ops = {
    .read = alchemist_mmio_read,
    .write = alchemist_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void pci_alchemist_realize(PCIDevice *pdev, Error **errp)
{
    AlchemistState *s = ALCHEMIST(pdev);

    s->mmio_buf = g_malloc0(ALCHEMIST_MMIO_SIZE);
    alchemist_vram_init(s);
    memory_region_init_io(&s->mmio, OBJECT(s), &alchemist_mmio_ops, s,
                           "alchemist-mmio", ALCHEMIST_MMIO_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                      PCI_BASE_ADDRESS_MEM_TYPE_64, &s->mmio);

    memory_region_init_ram(&s->vram, OBJECT(s), "alchemist-vram",
                            ALCHEMIST_VRAM_SIZE, &error_fatal);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY |
                      PCI_BASE_ADDRESS_MEM_TYPE_64 |
                      PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);
}

static void pci_alchemist_exit(PCIDevice *pdev)
{
    AlchemistState *s = ALCHEMIST(pdev);

    g_free(s->mmio_buf);
}

static void alchemist_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_alchemist_realize;
    k->exit = pci_alchemist_exit;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = ALCHEMIST_PCI_DEVICE_ID;
    k->revision = ALCHEMIST_PCI_REVISION;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo alchemist_types[] = {
    {
        .name          = TYPE_PCI_ALCHEMIST_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(AlchemistState),
        .class_init    = alchemist_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(alchemist_types)
