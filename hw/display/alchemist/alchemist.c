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
#include "hw/pci/msi.h"
#include "qom/object.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "alchemist_internal.h"

#define TYPE_PCI_ALCHEMIST_DEVICE "alchemist"
DECLARE_INSTANCE_CHECKER(AlchemistState, ALCHEMIST,
                          TYPE_PCI_ALCHEMIST_DEVICE)

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

    /*
     * The GT interrupt status registers are write-1-to-clear, not plain
     * memory - see alchemist_irq.c's file comment - so they're handled
     * entirely by alchemist_irq_status_write() instead of the generic
     * store every other register uses.
     */
    if (alchemist_irq_is_status_reg(addr)) {
        alchemist_irq_status_write(s, addr, val);
        return;
    }

    memcpy(s->mmio_buf + addr, &val, size);

    alchemist_pcode_mmio_write(s, addr, size);
    alchemist_forcewake_mmio_write(s, addr, size);
    alchemist_guc_mmio_write(s, addr, size);
    alchemist_irq_mmio_write(s, addr, size);
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
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY |
                      PCI_BASE_ADDRESS_MEM_TYPE_64 |
                      PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);

    /*
     * xe_irq_install() falls back to a single plain MSI vector whenever
     * pci_msix_vec_count() reports no MSI-X capability (-EINVAL) - see
     * xe_irq_msix_init() upstream - so that's all we need to provide.
     */
    pci_config_set_interrupt_pin(pdev->config, 1);
    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    /*
     * Non-fatal: nothing yet depends on the satellite process being up
     * (see alchemist_guc_proc.c) - a device that works exactly as it did
     * before this process existed is a better failure mode than losing
     * the whole GPU device over it.
     */
    {
        Error *local_err = NULL;

        if (!alchemist_guc_proc_start(s, &local_err)) {
            warn_report_err(local_err);
        }
    }
}

static void pci_alchemist_exit(PCIDevice *pdev)
{
    AlchemistState *s = ALCHEMIST(pdev);

    alchemist_guc_proc_stop(s);
    msi_uninit(pdev);
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
