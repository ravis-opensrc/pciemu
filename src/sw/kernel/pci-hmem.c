#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/numa.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/memregion.h>
#include <linux/dax.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>

#include "../../../include/hw/pciemu_hw.h"
#include "../../../include/sw/module/pciemu_ioctl.h"

#define DRV_MODULE_NAME "pci-hmem"

struct pci_hmem_prv {
	struct platform_device *platform_dev;
	long mem_id;
	resource_size_t bar0_size;
	resource_size_t bar0_start;
        struct miscdevice miscdev;
};

static int pci_hmem_probe(struct pci_dev *pdev,
		          const struct pci_device_id *id);
static void pci_hmem_remove(struct pci_dev *pdev);
static void release_memregion(void *data);
static void release_hmem(void *data);

static void release_memregion(void *data) {
	memregion_free((long)(data));
}

static void release_hmem(void *pdev) {
	platform_device_unregister(pdev);
}

static int pci_hmem_open(struct inode *inode, struct file *file);
static int pci_hmem_release(struct inode *inode, struct file *file);
static long pci_hmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int pci_hmem_mmap(struct file *file, struct vm_area_struct *vma);

static const struct file_operations pci_hmem_fops = {
    .owner = THIS_MODULE,
    .open = pci_hmem_open,
    .release = pci_hmem_release,
    .unlocked_ioctl = pci_hmem_ioctl,
    .mmap = pci_hmem_mmap,
};

static int pci_hmem_open(struct inode *inode, struct file *file) {
    struct pci_hmem_prv *pci_drv_data = container_of(file->private_data, struct pci_hmem_prv, miscdev);
    file->private_data = pci_drv_data;
    return 0;
}

static int pci_hmem_release(struct inode *inode, struct file *file) {
    return 0;
}

static long pci_hmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct pci_hmem_prv *pci_drv_data = file->private_data;
    size_t bar0_size;

    switch (cmd) {
        case PCIEMU_IOCTL_GET_BAR0_SIZE:
            bar0_size = pci_drv_data->bar0_size;
            if (copy_to_user((void __user *)arg, &bar0_size, sizeof(bar0_size)))
                return -EFAULT;
            break;
        default:
            return -EINVAL;
    }

    return 0;
}

static int pci_hmem_mmap(struct file *file, struct vm_area_struct *vma) {
    struct pci_hmem_prv *pci_drv_data = file->private_data;
    unsigned long pfn = (unsigned long)(pci_drv_data->bar0_start) >> PAGE_SHIFT;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > pci_drv_data->bar0_size)
        return -EINVAL;

    //vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot))
        return -EAGAIN;

    return 0;
}

static int pci_hmem_probe(struct pci_dev *pdev,
		          const struct pci_device_id *id) {
	struct device *dev = &pdev->dev;
	struct platform_device *platform_dev;
	int rc;
	struct memregion_info info;
	resource_size_t io_start, io_end, io_len;
	struct resource res;
	int target_id;
	struct pci_hmem_prv *pci_drv_data;

	dev_err(dev, "Entered probe\n");
	memset(&res, 0, sizeof(res));

	//enable the device
	rc = pcim_enable_device(pdev);
	if(rc) {
		dev_err(dev, "Failed to enable pci device rc:%d",rc);
		return rc;
	}

	//read bar register
	io_start = pci_resource_start(pdev, PCIE_MMIO_DAX_BAR_NUM);
	if(!io_start) {
		dev_err(dev, "No IO resource at BAR %d", PCIE_MMIO_DAX_BAR_NUM);
		return -1;
	}
	io_end = pci_resource_end(pdev, PCIE_MMIO_DAX_BAR_NUM);
	io_len = pci_resource_len(pdev, PCIE_MMIO_DAX_BAR_NUM);

	res.start = io_start;
	res.end = io_end;
	res.flags = IORESOURCE_MEM;

	target_id = phys_to_target_node(res.start);

	rc = region_intersects(res.start,
			resource_size(&res), IORESOURCE_MEM, IORES_DESC_NONE);
	if (rc != REGION_INTERSECTS) {
		dev_err(dev, "region intersects");
		return -1;
	}

	//Create driver private data
	pci_drv_data = (struct pci_hmem_prv *) devm_kzalloc(dev,
			sizeof(struct pci_hmem_prv), GFP_KERNEL);
	if(!pci_drv_data) {
		dev_err(dev, "driver prv data allocation failure");
		return -ENOMEM;
	}

	pci_drv_data->bar0_size = pci_resource_len(pdev, PCIEMU_HW_BAR0);
        pci_drv_data->bar0_start = pci_resource_start(pdev, PCIEMU_HW_BAR0);

	//platform device create and add
	pci_drv_data->mem_id = memregion_alloc(GFP_KERNEL);
	if (pci_drv_data->mem_id < 0) {
		dev_err(dev, "memregion allocation failure for %pr\n", &res);
		return -ENOMEM;
	}

        rc = devm_add_action_or_reset(dev, release_memregion, (void *)(pci_drv_data->mem_id));

	platform_dev = platform_device_alloc("hmem", pci_drv_data->mem_id);
	if (!platform_dev) {
		dev_err(dev, "hmem device allocation failure for %pr\n", &res);
		return -ENOMEM;
	}

	platform_dev->dev.numa_node = numa_map_to_online_node(target_id);
	info = (struct memregion_info) {
		.target_node = target_id,
		.range = {
                        .start = res.start,
                        .end = res.end,
                },
	};

	platform_dev->dev.parent = dev;

	rc = platform_device_add_data(platform_dev, &info, sizeof(info));
	if (rc < 0) {
		dev_err(dev, "hmem memregion_info allocation failure for %pr\n", &res);
		goto out_put;
	}

	rc = platform_device_add_resources(platform_dev, &res, 1);
	if (rc < 0) {
		dev_err(dev, "hmem resource allocation failure for %pr\n", &res);
		goto out_put;
	}

	rc = platform_device_add(platform_dev);
	if (rc < 0) {
		dev_err(dev, "device add failed for %pr\n", &res);
		goto out_put;
	}

	pci_drv_data->platform_dev = platform_dev;

	pci_set_drvdata(pdev, pci_drv_data);

        // Register the character device
        pci_drv_data->miscdev.minor = MISC_DYNAMIC_MINOR;
        pci_drv_data->miscdev.name = DRV_MODULE_NAME;
        pci_drv_data->miscdev.fops = &pci_hmem_fops;

        rc = misc_register(&pci_drv_data->miscdev);
        if (rc) {
            dev_err(dev, "Failed to register misc device");
            goto out_put;
        }

	dev_err(dev, "pci-hmem probe completed successfully");

	return devm_add_action_or_reset(dev, release_hmem, platform_dev);

out_put:
	platform_device_put(platform_dev);

	return -1;
}

static void pci_hmem_remove(struct pci_dev *pdev) {
	struct pci_hmem_prv *private_data;

	private_data = (struct pci_hmem_prv *) pci_get_drvdata(pdev);

 	if(private_data) {
		misc_deregister(&private_data->miscdev);
 	}

	dev_err(&pdev->dev, "Return from %s",__func__);
}

static struct pci_device_id pci_hmem_ids[] = {
	{ PCI_DEVICE(PCIEMU_HW_VENDOR_ID, PCIEMU_HW_DEVICE_ID), },
	{0, }
};

MODULE_DEVICE_TABLE(pci, pci_hmem_ids);

static struct pci_driver pci_hmem_driver = {
	.name = DRV_MODULE_NAME,
	.id_table = pci_hmem_ids,
	.probe = pci_hmem_probe,
	.remove = pci_hmem_remove,
};

module_pci_driver(pci_hmem_driver);

MODULE_AUTHOR("MICRON DMC");
MODULE_DESCRIPTION("HMEM PCIe BAR Mapped I/O driver");
MODULE_LICENSE("GPL v2");
