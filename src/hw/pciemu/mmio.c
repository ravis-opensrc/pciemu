/* mmio.c - Memory Mapped IO operations
 *
 * Official documentation on MMIO and memory operations can be found in :
 *    https://qemu.readthedocs.io/en/latest/devel/memory.html
 *
 * Copyright (c) 2023 Luiz Henrique Suraty Filho <luiz-dev@suraty.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 */
#include "qemu/osdep.h"
#include "exec/target_page.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "mmio.h"
#include "irq.h"
#include "pciemu_hw.h"
#include "trace.h"

/* -----------------------------------------------------------------------------
 *  Private
 * -----------------------------------------------------------------------------
 */

/**
 * pciemu_mmio_valid_access: Check whether the access is valid
 *
 * The size verification here is not required.
 * (memory_region_access_valid function in QEMU core will filter those out)
 *
 * @addr: address being accessed (relative to the Memory Region)
 * @size: read size in bytes (4 or 8)
 */
static inline bool pciemu_mmio_valid_access(PCIEMUDevice *dev, hwaddr addr, unsigned int size)
{
    if (addr >= dev->bar0_start && addr < dev->bar0_start + (dev->num_regs * sizeof(uint32_t))) {
        // Check alignment for BAR0 (uint32_t)
        if (addr % sizeof(uint32_t) == 0 && size == sizeof(uint32_t)) {
            return true;
        }
    } else if (addr >= dev->bar2_start && addr < dev->bar2_start + (dev->bar2_size_mb * MiB)) {
        // Check alignment for BAR2 (uint64_t)
        if (addr % sizeof(uint64_t) == 0 && size == sizeof(uint64_t)) {
            return true;
        }
    }
    return false;
}

/**
 * pciemu_mmio_read: Callback for read operations
 *
 * Read from the memory region and return the correspondent value.
 * Only valid for regions with READ operations (mostly regiters)
 *
 * @opaque: opaque pointer that points to instantiated object
 * @addr: address being accessed (relative to the Memory Region)
 * @size: read size in bytes (4 or 8)
 */
static uint64_t pciemu_mmio_read(void *opaque, hwaddr addr, unsigned int size)
{
    PCIEMUDevice *dev = opaque;
    uint64_t val = ~0ULL;
    uint8_t bar=0;
    uint64_t address = (uint64_t)addr;

    if (!pciemu_mmio_valid_access(dev, addr, size))
        return val;

    if (addr >= dev->bar0_start && addr < dev->bar0_start + (dev->num_regs * sizeof(uint32_t))) {
        // Handle BAR0 (uint32_t)
        val = (uint32_t)(dev->bar0_regs[(addr - dev->bar0_start) / sizeof(uint32_t)]);
	bar = 0;
    } else if (addr >= dev->bar2_start && addr < dev->bar2_start + (dev->bar2_size_mb * MiB)) {
        // Handle BAR2 (uint64_t)
        val = dev->bar2_mem[(addr - dev->bar2_start) / sizeof(uint64_t)];
	bar = 2;
    }

    trace_pciemu_mmio_read(PCI_BUS_NUM(dev->pci_dev.devfn), PCI_SLOT(dev->pci_dev.devfn), PCI_FUNC(dev->pci_dev.devfn), bar, address, size, val);

    return val;
}

/**
 * pciemu_mmio_write: Callback for write operations
 *
 * Write to the memory region.
 * For now, it writes to reg0 regardless of the address.
 *
 * @opaque: opaque pointer that points to instantiated object
 * @addr: address being written (relative to the Memory Region)
 * @val: value to be written
 * @size: write size in bytes (4 or 8)
 */
static void pciemu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    PCIEMUDevice *dev = opaque;
    uint8_t bar=0;
    uint64_t address = (uint64_t)addr;

    if (!pciemu_mmio_valid_access(dev, addr, size))
        return;

    if (addr >= dev->bar0_start && addr < dev->bar0_start + (dev->num_regs * sizeof(uint32_t))) {
        // Handle BAR0 (uint32_t)
        dev->bar0_regs[(addr - dev->bar0_start) / sizeof(uint32_t)] = (uint32_t)val;
	bar = 0;
    } else if (addr >= dev->bar2_start && addr < dev->bar2_start + (dev->bar2_size_mb * MiB)) {
        // Handle BAR2 (uint64_t)
        dev->bar2_mem[(addr - dev->bar2_start) / sizeof(uint64_t)] = val;
	bar = 2;
    }
    trace_pciemu_mmio_write(PCI_BUS_NUM(dev->pci_dev.devfn), PCI_SLOT(dev->pci_dev.devfn), PCI_FUNC(dev->pci_dev.devfn), bar, address, size, val);
}

/* -----------------------------------------------------------------------------
 *  Public
 * -----------------------------------------------------------------------------
 */

/**
 * pciemu_mmio_reset: MMIO reset
 *
 * As the mmio block controls the device registers (reg),
 * we just clean them up here.
 *
 * @dev: Instance of PCIEMUDevice object being used
 */
void pciemu_mmio_reset(PCIEMUDevice *dev)
{
    for (int i = 0; i < dev->num_regs; ++i)
        dev->bar0_regs[i] = 0;

    memset(dev->bar2_mem, 0, dev->bar2_size_mb * MiB);
}

#define BAR_SIZE(bar_size, target_page_size) \
    ((bar_size) < (target_page_size) ? (target_page_size) : \
    (((bar_size) + (target_page_size) - 1) / (target_page_size)) * (target_page_size))

/**
 * pciemu_mmio_init: MMIO initialization
 *
 * Initializes the MMIO block for the instantiated PCIEMUDevice object.
 * Note that we receive a pointer for a PCIEMUDevice, but, due to the OOP hack
 * done by the QEMU Object Model, we can easily get the parent PCIDevice.
 *
 * @dev: Instance of PCIEMUDevice object being initialized
 * @errp: pointer to indicate errors
 */
void pciemu_mmio_init(PCIEMUDevice *dev, Error **errp)
{
    uint64_t bar0_req_size, bar0_size;
    uint64_t bar2_req_size, bar2_size;
    int target_page_size;

    bar0_req_size = dev->num_regs * sizeof(uint32_t);
    target_page_size = qemu_target_page_size();
    bar0_size = BAR_SIZE(bar0_req_size, target_page_size);

    /* Initialize BAR 0 for register access */
    memory_region_init_io(&dev->bar0, OBJECT(dev), &pciemu_mmio_ops, dev, "pciemu-mmio-bar0", bar0_size);
    pci_register_bar(&dev->pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &dev->bar0);
    dev->bar0_start = pci_get_bar_addr(&dev->pci_dev, 0);

    bar2_req_size = dev->bar2_size_mb * MiB;
    bar2_size = BAR_SIZE(bar2_req_size, target_page_size);

    /* Initialize BAR 2 for memory access */
    memory_region_init_io(&dev->bar2, OBJECT(dev), &pciemu_mmio_ops, dev, "pciemu-mmio-bar2", bar2_size);
    pci_register_bar(&dev->pci_dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &dev->bar2);
    dev->bar2_start = pci_get_bar_addr(&dev->pci_dev, 2);

    dev->bar0_regs = g_new0(uint32_t, dev->num_regs);
    dev->bar2_mem = g_new0(uint64_t, dev->bar2_size_mb * (MiB / sizeof(uint64_t)));
}

/**
 * pciemu_mmio_fini: MMIO finalization
 *
 * Finalizes the MMIO block for the instantiated PCIEMUDevice object.
 * Note that we receive a pointer for a PCIEMUDevice, but, due to the OOP hack
 * done by the QEMU Object Model, we can easily get the parent PCIDevice.
 *
 * @dev: Instance of PCIEMUDevice object being finalized
 */
void pciemu_mmio_fini(PCIEMUDevice *dev)
{
    pciemu_mmio_reset(dev);
}

/**
 * pciemu_mmio_ops: Memory region description
 *
 * Describes the operations and behavior (with callbacks)
 * of the device memory region dedicate for MMIO.
 *
 */
const MemoryRegionOps pciemu_mmio_ops = {
    .read = pciemu_mmio_read,
    .write = pciemu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
	.min_access_size = 4,
	.max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};
