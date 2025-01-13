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
#include "network_initiator.h"
#include "transaction.h"

/* -----------------------------------------------------------------------------
 *  Private
 * -----------------------------------------------------------------------------
 */

/**
 * pciemu_bar0_valid_access: Check whether the access is valid for BAR0
 *
 * @dev: Pointer to PCIEMUDevice private structure
 * @addr: address being accessed (relative to the BAR0)
 * @size: read size in bytes (4)
 */
static bool pciemu_bar0_valid_access(PCIEMUDevice *dev, hwaddr addr, unsigned int size)
{
    if (addr < (dev->num_regs * sizeof(uint32_t)) && addr % sizeof(uint32_t) == 0 && size == sizeof(uint32_t)) {
        return true;
    }
    return false;
}

/**
 * pciemu_bar2_valid_access: Check whether the access is valid for BAR2
 *
 * @dev: Pointer to PCIEMUDevice private structure
 * @addr: address being accessed (relative to the BAR2)
 * @size: read size in bytes (1, 2, 4, or 8)
 */
static bool pciemu_bar2_valid_access(PCIEMUDevice *dev, hwaddr addr, unsigned int size)
{
    if (addr < (dev->bar2_size_mb * MiB)) {
        return true;
    }
    return false;
}

/**
 * pciemu_bar0_read: Callback for read operations on BAR0
 *
 * @opaque: opaque pointer that points to instantiated object
 * @addr: address being accessed (relative to the BAR0)
 * @size: read size in bytes (4)
 */
static uint64_t pciemu_bar0_read(void *opaque, hwaddr addr, unsigned int size)
{
    PCIEMUDevice *dev = opaque;
    uint64_t val = ~0ULL;
    uint8_t bar = 0;
    uint64_t address = (uint64_t)addr;

    if (!pciemu_bar0_valid_access(dev, addr, size))
        return val;

#ifdef NETWORK_TLM 
    TransactionHeader trans = { .type = READ, .address = addr / sizeof(uint32_t) };
    TransactionNode node;
    node.header = trans;
    node.completed = 0;
    pthread_cond_init(&node.cond, NULL);
    pthread_mutex_init(&node.mutex, NULL);

    network_initiator_send_transaction(dev->initiator, &node);

    pthread_mutex_lock(&node.mutex);
    while (!node.completed) {
        pthread_cond_wait(&node.cond, &node.mutex);
    }
    pthread_mutex_unlock(&node.mutex);
    val = (uint32_t)(node.header.data);
#else
    val = (uint32_t)(dev->bar0_regs[addr / sizeof(uint32_t)]);
#endif

    trace_pciemu_mmio_read(PCI_BUS_NUM(dev->pci_dev.devfn), PCI_SLOT(dev->pci_dev.devfn), PCI_FUNC(dev->pci_dev.devfn), bar, address, size, val);
    return val;
}

/**
 * pciemu_bar0_write: Callback for write operations on BAR0
 *
 * @opaque: opaque pointer that points to instantiated object
 * @addr: address being written (relative to the BAR0)
 * @val: value to be written
 * @size: write size in bytes (4)
 */
static void pciemu_bar0_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    PCIEMUDevice *dev = opaque;
    uint8_t bar = 0;
    uint64_t address = (uint64_t)addr;

    if (!pciemu_bar0_valid_access(dev, addr, size))
        return;

#ifdef NETWORK_TLM
    TransactionHeader trans = { .type = WRITE, .address = addr / sizeof(uint32_t), .data = val };

    TransactionNode node;
    node.header = trans;
    node.completed = 0;
    pthread_cond_init(&node.cond, NULL);
    pthread_mutex_init(&node.mutex, NULL);

    network_initiator_send_transaction(dev->initiator, &node);

    pthread_mutex_lock(&node.mutex);
    while (!node.completed) {
        pthread_cond_wait(&node.cond, &node.mutex);
    }
    pthread_mutex_unlock(&node.mutex);
#else
    dev->bar0_regs[addr / sizeof(uint32_t)] = (uint32_t)val;
#endif

    trace_pciemu_mmio_write(PCI_BUS_NUM(dev->pci_dev.devfn), PCI_SLOT(dev->pci_dev.devfn), PCI_FUNC(dev->pci_dev.devfn), bar, address, size, val);
}

/**
 * pciemu_bar2_read: Callback for read operations on BAR2
 *
 * @opaque: opaque pointer that points to instantiated object
 * @addr: address being accessed (relative to the BAR2)
 * @size: read size in bytes (1, 2, 4, or 8)
 */
static uint64_t pciemu_bar2_read(void *opaque, hwaddr addr, unsigned int size)
{
    PCIEMUDevice *dev = opaque;
    uint64_t val = ~0ULL;
    uint8_t bar = 2;
    uint64_t address = (uint64_t)addr;

    if (!pciemu_bar2_valid_access(dev, addr, size))
        return val;

    memcpy(&val, &dev->bar2_mem[addr], size);

    trace_pciemu_mmio_read(PCI_BUS_NUM(dev->pci_dev.devfn), PCI_SLOT(dev->pci_dev.devfn), PCI_FUNC(dev->pci_dev.devfn), bar, address, size, val);
    return val;
}

/**
 * pciemu_bar2_write: Callback for write operations on BAR2
 *
 * @opaque: opaque pointer that points to instantiated object
 * @addr: address being written (relative to the BAR2)
 * @val: value to be written
 * @size: write size in bytes (1, 2, 4, or 8)
 */
static void pciemu_bar2_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    PCIEMUDevice *dev = opaque;
    uint8_t bar = 2;
    uint64_t address = (uint64_t)addr;

    if (!pciemu_bar2_valid_access(dev, addr, size))
        return;

    memcpy(&dev->bar2_mem[addr], &val, size);

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
 * pciemu_bar0_ops: Memory region description for BAR0
 *
 * Describes the operations and behavior (with callbacks)
 * of the device memory region dedicated for BAR0.
 *
 */
const MemoryRegionOps pciemu_bar0_ops = {
    .read = pciemu_bar0_read,
    .write = pciemu_bar0_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/**
 * pciemu_bar2_ops: Memory region description for BAR2
 *
 * Describes the operations and behavior (with callbacks)
 * of the device memory region dedicated for BAR2.
 *
 */
const MemoryRegionOps pciemu_bar2_ops = {
    .read = pciemu_bar2_read,
    .write = pciemu_bar2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

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

#ifdef NETWORK_TLM 
    dev->initiator =  network_initiator_new(); 
    network_initiator_initialize(dev->initiator, "127.0.0.1", SERVER_PORT);
#endif

    bar0_req_size = dev->num_regs * sizeof(uint32_t);
    target_page_size = qemu_target_page_size();
    bar0_size = BAR_SIZE(bar0_req_size, target_page_size);


    /* Initialize BAR 0 for register access */
    memory_region_init_io(&dev->bar0, OBJECT(dev), &pciemu_bar0_ops, dev, "pciemu-mmio-bar0", bar0_size);
    pci_register_bar(&dev->pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &dev->bar0);
    dev->bar0_start = pci_get_bar_addr(&dev->pci_dev, 0);

    bar2_req_size = dev->bar2_size_mb * MiB;
    bar2_size = BAR_SIZE(bar2_req_size, target_page_size);

    /* Initialize BAR 2 for memory access */
    memory_region_init_io(&dev->bar2, OBJECT(dev), &pciemu_bar2_ops, dev, "pciemu-mmio-bar2", bar2_size);
    pci_register_bar(&dev->pci_dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &dev->bar2);
    dev->bar2_start = pci_get_bar_addr(&dev->pci_dev, 2);

    dev->bar0_regs = g_new0(uint32_t, dev->num_regs);
    dev->bar2_mem = g_new0(char, dev->bar2_size_mb * MiB);
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
