/*
 * QTest testcase for the invertram PCI device
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"

static void test_invert_dword(void)
{
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    QPCIBar bar;
    uint32_t val;

    qts = qtest_init("-device invertram,addr=04.0");
    pcibus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(pcibus, QPCI_DEVFN(0x4, 0x0));
    qpci_device_enable(dev);
    bar = qpci_iomap(dev, 0, NULL);

    qpci_memwrite(dev, bar, 0, &(uint32_t){ 0x12345678 }, sizeof(uint32_t));
    qpci_memread(dev, bar, 0, &val, sizeof(val));
    g_assert_cmpuint(val, ==, ~0x12345678U);

    qpci_memwrite(dev, bar, 4, &(uint32_t){ 0x00000000 }, sizeof(uint32_t));
    qpci_memread(dev, bar, 4, &val, sizeof(val));
    g_assert_cmpuint(val, ==, 0xFFFFFFFFU);

    qpci_memwrite(dev, bar, 8, &(uint32_t){ 0xFFFFFFFF }, sizeof(uint32_t));
    qpci_memread(dev, bar, 8, &val, sizeof(val));
    g_assert_cmpuint(val, ==, 0x00000000U);

    g_free(dev);
    qpci_free_pc(pcibus);
    qtest_quit(qts);
}

static void test_invert_byte(void)
{
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    QPCIBar bar;
    uint8_t val;

    qts = qtest_init("-device invertram,addr=04.0");
    pcibus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(pcibus, QPCI_DEVFN(0x4, 0x0));
    qpci_device_enable(dev);
    bar = qpci_iomap(dev, 0, NULL);

    qpci_memwrite(dev, bar, 0x10, &(uint8_t){ 0xAA }, sizeof(uint8_t));
    qpci_memread(dev, bar, 0x10, &val, sizeof(val));
    g_assert_cmpuint(val, ==, (uint8_t)~0xAA);

    g_free(dev);
    qpci_free_pc(pcibus);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/invertram/invert-dword", test_invert_dword);
    qtest_add_func("/invertram/invert-byte", test_invert_byte);

    return g_test_run();
}
