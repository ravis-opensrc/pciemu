/* pciemu.h
 *
 * Copyright (c) 2023 Luiz Henrique Suraty Filho <luiz-dev@suraty.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 */
#ifndef PCIEMU_H
#define PCIEMU_H

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "pciemu_hw.h"
#include "dma.h"
#include "irq.h"

#define TYPE_PCIEMU_DEVICE "pciemu"
#define PCIEMU_DEVICE_DESC "PCIEMU Device"
/*
 * Declare the object type for PCIEMUDevice and all boilerplate code
 * See https://qemu.readthedocs.io/en/latest/devel/qom.html for details
 *
 */
OBJECT_DECLARE_TYPE(PCIEMUDevice, PCIEMUDeviceClass, PCIEMU_DEVICE);

/* Struct that defines our class
 */
typedef struct PCIEMUDeviceClass {
    /*
     * OOP hack : Our parent class (PCIDeviceClass) is part of the strutct
     * The idea is to be able to access it from our own class
     */
    PCIDeviceClass parent_class;
} PCIEMUDeviceClass;

typedef struct PCIEMUDevice {
    /*< private >*/
    PCIDevice pci_dev;
    /*< public >*/

    /* IRQs */
    IRQStatus irq;

    /* DMAs */
    DMAEngine dma;

    /* Memory Regions */
    MemoryRegion bar0; /* BAR 0 (registers) */
    MemoryRegion bar2; /* BAR 2 (memory) */

    /* Registers in BAR0 */
    uint32_t *bar0_regs;
    /* Memory in BAR2 */
    char *bar2_mem;

    /* Number of registers in BAR0 */
    uint32_t num_regs;
    /* Size of BAR2 in MB */
    uint32_t bar2_size_mb;

    /* Start addresses of BARs */
    hwaddr bar0_start;
    hwaddr bar2_start;

} PCIEMUDevice;

#endif /* PCIEMU_H */
