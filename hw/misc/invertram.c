/*
 * QEMU invertram PCI device
 *
 * A minimal educational PCI device: it exposes a small MMIO BAR. Any write
 * to an offset is bitwise-inverted and stored; a read of that offset
 * returns the stored (already-inverted) value.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"
#include "qemu/module.h"

#define TYPE_PCI_INVERTRAM_DEVICE "invertram"
typedef struct InvertRamState InvertRamState;
DECLARE_INSTANCE_CHECKER(InvertRamState, INVERTRAM,
                          TYPE_PCI_INVERTRAM_DEVICE)

#define INVERTRAM_BAR_SIZE (4 * 1024)

struct InvertRamState {
    PCIDevice pdev;
    MemoryRegion mmio;
    uint8_t data[INVERTRAM_BAR_SIZE];
};

static uint64_t invertram_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    InvertRamState *s = opaque;
    uint64_t val = 0;

    memcpy(&val, s->data + addr, size);
    return val;
}

static void invertram_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    InvertRamState *s = opaque;
    uint64_t inverted = ~val;

    memcpy(s->data + addr, &inverted, size);
}

static const MemoryRegionOps invertram_mmio_ops = {
    .read = invertram_mmio_read,
    .write = invertram_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void pci_invertram_realize(PCIDevice *pdev, Error **errp)
{
    InvertRamState *s = INVERTRAM(pdev);

    memory_region_init_io(&s->mmio, OBJECT(s), &invertram_mmio_ops, s,
                           "invertram-mmio", INVERTRAM_BAR_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void invertram_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_invertram_realize;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = 0x11e9;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo invertram_types[] = {
    {
        .name          = TYPE_PCI_INVERTRAM_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(InvertRamState),
        .class_init    = invertram_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(invertram_types)
