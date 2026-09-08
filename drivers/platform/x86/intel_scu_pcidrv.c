// SPDX-License-Identifier: GPL-2.0
/*
 * PCI driver for the Intel SCU.
 *
 * Copyright (C) 2008-2010, 2015, 2020 Intel Corporation
 * Authors: Sreedhara DS (sreedhara.ds@intel.com)
 *	    Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <linux/mfd/intel_msic.h>
#include <linux/platform_data/x86/intel_scu_ipc.h>

#include <asm/intel-mid.h>
#include <asm/io_apic.h>

struct intel_msic_platform_data msic_pdata;

static struct intel_msic_gpio_pdata msic_gpio_pdata = {
	.gpio_base = 192,
};

static struct resource msic_resources[] = {
	{
		.start	= INTEL_MSIC_IRQ_PHYS_BASE,
		.end	= INTEL_MSIC_IRQ_PHYS_BASE + 64 - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device msic_device = {
	.name		= "intel_msic",
	.id		= -1,
	.dev		= {
		.platform_data	= &msic_pdata,
	},
	.num_resources	= ARRAY_SIZE(msic_resources),
	.resource	= msic_resources,
};

static int intel_scu_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	struct intel_scu_ipc_data scu_data = {};
	struct intel_scu_ipc_dev *scu;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	scu_data.mem = pdev->resource[0];
	scu_data.irq = pdev->irq;

	scu = intel_scu_ipc_register(&pdev->dev, &scu_data);

	msic_pdata.gpio = &msic_gpio_pdata;
	msic_pdata.irq[INTEL_MSIC_BLOCK_POWER_BTN] = mp_map_gsi_to_irq(67, IOAPIC_MAP_ALLOC, NULL);
	msic_pdata.irq[INTEL_MSIC_BLOCK_ADC] = mp_map_gsi_to_irq(16, IOAPIC_MAP_ALLOC, NULL);
	msic_pdata.irq[INTEL_MSIC_BLOCK_BATTERY] = mp_map_gsi_to_irq(17, IOAPIC_MAP_ALLOC, NULL);
	msic_pdata.irq[INTEL_MSIC_BLOCK_GPIO] = mp_map_gsi_to_irq(18, IOAPIC_MAP_ALLOC, NULL);
	platform_device_register(&msic_device);

	return PTR_ERR_OR_ZERO(scu);
}

static const struct pci_device_id pci_ids[] = {
	{ PCI_VDEVICE(INTEL, 0x080e) },
	{ PCI_VDEVICE(INTEL, 0x082a) },
	{ PCI_VDEVICE(INTEL, 0x08ea) },
	{ PCI_VDEVICE(INTEL, 0x0a94) },
	{ PCI_VDEVICE(INTEL, 0x11a0) },
	{ PCI_VDEVICE(INTEL, 0x1a94) },
	{ PCI_VDEVICE(INTEL, 0x5a94) },
	{}
};

static struct pci_driver intel_scu_pci_driver = {
	.driver = {
		.suppress_bind_attrs = true,
	},
	.name = "intel_scu",
	.id_table = pci_ids,
	.probe = intel_scu_pci_probe,
};

builtin_pci_driver(intel_scu_pci_driver);
