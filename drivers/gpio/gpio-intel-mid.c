// SPDX-License-Identifier: GPL-2.0
/*
 * Intel MID GPIO driver
 *
 * Copyright (c) 2008-2014,2016 Intel Corporation.
 */

/* Supports:
 * Moorestown platform Langwell chip.
 * Medfield platform Penwell chip.
 * Clovertrail platform Cloverview chip.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/stddef.h>

#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <asm/processor.h>

#define INTEL_MID_IRQ_TYPE_EDGE		(1 << 0)
#define INTEL_MID_IRQ_TYPE_LEVEL	(1 << 1)

/*
 * Langwell chip has 64 pins and thus there are 2 32bit registers to control
 * each feature, while Penwell chip has 96 pins for each block, and need 3 32bit
 * registers to control them, so we only define the order here instead of a
 * structure, to get a bit offset for a pin (use GPDR as an example):
 *
 * nreg = ngpio / 32;
 * reg = offset / 32;
 * bit = offset % 32;
 * reg_addr = reg_base + GPDR * nreg * 4 + reg * 4;
 *
 * so the bit of reg_addr is to control pin offset's GPDR feature
*/

enum GPIO_REG {
	GPLR = 0,	/* pin level read-only */
	GPDR,		/* pin direction */
	GPSR,		/* pin set */
	GPCR,		/* pin clear */
	GRER,		/* rising edge detect */
	GFER,		/* falling edge detect */
	GEDR,		/* edge detect result */
	GAFR,		/* alt function */
};

/* intel_mid gpio driver data */
struct intel_mid_gpio_ddata {
	u16 ngpio;		/* number of gpio pins */
	u32 chip_irq_type;	/* chip interrupt type */
	u32 gpio_base;		/* first gpio number (fallback if BAR1 absent) */
	u32 irq_base;		/* first irq number (fallback if BAR1 absent) */
};

struct intel_mid_gpio {
	struct gpio_chip		chip;
	void __iomem			*reg_base;
	spinlock_t			lock;
	struct pci_dev			*pdev;
};

static void __iomem *gpio_reg(struct gpio_chip *chip, unsigned offset,
			      enum GPIO_REG reg_type)
{
	struct intel_mid_gpio *priv = gpiochip_get_data(chip);
	unsigned nreg = chip->ngpio / 32;
	u8 reg = offset / 32;

	return priv->reg_base + reg_type * nreg * 4 + reg * 4;
}

static void __iomem *gpio_reg_2bit(struct gpio_chip *chip, unsigned offset,
				   enum GPIO_REG reg_type)
{
	struct intel_mid_gpio *priv = gpiochip_get_data(chip);
	unsigned nreg = chip->ngpio / 32;
	u8 reg = offset / 16;

	return priv->reg_base + reg_type * nreg * 4 + reg * 4;
}

static int intel_gpio_request(struct gpio_chip *chip, unsigned offset)
{
	void __iomem *gafr = gpio_reg_2bit(chip, offset, GAFR);
	u32 value = readl(gafr);
	int shift = (offset % 16) << 1, af = (value >> shift) & 3;

	if (af) {
		value &= ~(3 << shift);
		writel(value, gafr);
	}
	return 0;
}

static int intel_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	void __iomem *gplr = gpio_reg(chip, offset, GPLR);

	return !!(readl(gplr) & BIT(offset % 32));
}

static int intel_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	void __iomem *gpsr, *gpcr;

	if (value) {
		gpsr = gpio_reg(chip, offset, GPSR);
		writel(BIT(offset % 32), gpsr);
	} else {
		gpcr = gpio_reg(chip, offset, GPCR);
		writel(BIT(offset % 32), gpcr);
	}

	return 0;
}

static int intel_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct intel_mid_gpio *priv = gpiochip_get_data(chip);
	void __iomem *gpdr = gpio_reg(chip, offset, GPDR);
	u32 value;
	unsigned long flags;

	if (priv->pdev)
		pm_runtime_get(&priv->pdev->dev);

	spin_lock_irqsave(&priv->lock, flags);
	value = readl(gpdr);
	value &= ~BIT(offset % 32);
	writel(value, gpdr);
	spin_unlock_irqrestore(&priv->lock, flags);

	if (priv->pdev)
		pm_runtime_put(&priv->pdev->dev);

	return 0;
}

static int intel_gpio_direction_output(struct gpio_chip *chip,
			unsigned offset, int value)
{
	struct intel_mid_gpio *priv = gpiochip_get_data(chip);
	void __iomem *gpdr = gpio_reg(chip, offset, GPDR);
	unsigned long flags;

	intel_gpio_set(chip, offset, value);

	if (priv->pdev)
		pm_runtime_get(&priv->pdev->dev);

	spin_lock_irqsave(&priv->lock, flags);
	value = readl(gpdr);
	value |= BIT(offset % 32);
	writel(value, gpdr);
	spin_unlock_irqrestore(&priv->lock, flags);

	if (priv->pdev)
		pm_runtime_put(&priv->pdev->dev);

	return 0;
}

static int intel_mid_irq_type(struct irq_data *d, unsigned type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct intel_mid_gpio *priv = gpiochip_get_data(gc);
	u32 gpio = irqd_to_hwirq(d);
	unsigned long flags;
	u32 value;
	void __iomem *grer = gpio_reg(&priv->chip, gpio, GRER);
	void __iomem *gfer = gpio_reg(&priv->chip, gpio, GFER);

	if (gpio >= priv->chip.ngpio)
		return -EINVAL;

	if (priv->pdev)
		pm_runtime_get(&priv->pdev->dev);

	spin_lock_irqsave(&priv->lock, flags);
	if (type & IRQ_TYPE_EDGE_RISING)
		value = readl(grer) | BIT(gpio % 32);
	else
		value = readl(grer) & (~BIT(gpio % 32));
	writel(value, grer);

	if (type & IRQ_TYPE_EDGE_FALLING)
		value = readl(gfer) | BIT(gpio % 32);
	else
		value = readl(gfer) & (~BIT(gpio % 32));
	writel(value, gfer);
	spin_unlock_irqrestore(&priv->lock, flags);

	if (priv->pdev)
		pm_runtime_put(&priv->pdev->dev);

	return 0;
}

static void intel_mid_irq_unmask(struct irq_data *d)
{
}

static void intel_mid_irq_mask(struct irq_data *d)
{
}

static struct irq_chip intel_mid_irqchip = {
	.name		= "INTEL_MID-GPIO",
	.irq_mask	= intel_mid_irq_mask,
	.irq_unmask	= intel_mid_irq_unmask,
	.irq_set_type	= intel_mid_irq_type,
};

static const struct intel_mid_gpio_ddata gpio_lincroft = {
	.ngpio = 64,
};

static const struct intel_mid_gpio_ddata gpio_penwell_aon = {
	.ngpio = 96,
	.chip_irq_type = INTEL_MID_IRQ_TYPE_EDGE,
};

static const struct intel_mid_gpio_ddata gpio_penwell_core = {
	.ngpio = 96,
	.chip_irq_type = INTEL_MID_IRQ_TYPE_EDGE,
};

static const struct intel_mid_gpio_ddata gpio_cloverview_aon = {
	.ngpio = 96,
	.chip_irq_type = INTEL_MID_IRQ_TYPE_EDGE | INTEL_MID_IRQ_TYPE_LEVEL,
	.gpio_base = 0,
	.irq_base = 256,
};

static const struct intel_mid_gpio_ddata gpio_cloverview_core = {
	.ngpio = 96,
	.chip_irq_type = INTEL_MID_IRQ_TYPE_EDGE,
	.gpio_base = 96,
	.irq_base = 352,
};

static const struct pci_device_id intel_gpio_ids[] = {
	{
		/* Lincroft */
		PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x080f),
		.driver_data = (kernel_ulong_t)&gpio_lincroft,
	},
	{
		/* Penwell AON */
		PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x081f),
		.driver_data = (kernel_ulong_t)&gpio_penwell_aon,
	},
	{
		/* Penwell Core */
		PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x081a),
		.driver_data = (kernel_ulong_t)&gpio_penwell_core,
	},
	{
		/* Cloverview Aon */
		PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x08eb),
		.driver_data = (kernel_ulong_t)&gpio_cloverview_aon,
	},
	{
		/* Cloverview Core */
		PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x08f7),
		.driver_data = (kernel_ulong_t)&gpio_cloverview_core,
	},
	{ }
};

static void intel_mid_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct intel_mid_gpio *priv = gpiochip_get_data(gc);
	struct irq_data *data = irq_desc_get_irq_data(desc);
	struct irq_chip *chip = irq_data_get_irq_chip(data);
	u32 base, gpio, mask;
	unsigned long pending;
	void __iomem *gedr;

	/* check GPIO controller to check which pin triggered the interrupt */
	for (base = 0; base < priv->chip.ngpio; base += 32) {
		gedr = gpio_reg(&priv->chip, base, GEDR);
		while ((pending = readl(gedr))) {
			gpio = __ffs(pending);
			mask = BIT(gpio);
			/* Clear before handling so we can't lose an edge */
			writel(mask, gedr);
			generic_handle_irq(irq_find_mapping(gc->irq.domain,
							    base + gpio));
		}
	}

	chip->irq_eoi(data);
}

static int intel_mid_irq_init_hw(struct gpio_chip *chip)
{
	struct intel_mid_gpio *priv = gpiochip_get_data(chip);
	void __iomem *reg;
	unsigned base;

	for (base = 0; base < priv->chip.ngpio; base += 32) {
		/* Clear the rising-edge detect register */
		reg = gpio_reg(&priv->chip, base, GRER);
		writel(0, reg);
		/* Clear the falling-edge detect register */
		reg = gpio_reg(&priv->chip, base, GFER);
		writel(0, reg);
		/* Clear the edge detect status register */
		reg = gpio_reg(&priv->chip, base, GEDR);
		writel(~0, reg);
	}

	return 0;
}

static int __maybe_unused intel_gpio_runtime_idle(struct device *dev)
{
	int err = pm_schedule_suspend(dev, 500);
	return err ?: -EBUSY;
}

static const struct dev_pm_ops intel_gpio_pm_ops = {
	SET_RUNTIME_PM_OPS(NULL, NULL, intel_gpio_runtime_idle)
};

static int intel_gpio_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	struct intel_mid_gpio *priv;
	int retval;
	struct gpio_irq_chip *girq;
	struct intel_mid_gpio_ddata *ddata =
				(struct intel_mid_gpio_ddata *)id->driver_data;
	u32 gpio_base = ddata->gpio_base;
	u32 irq_base = ddata->irq_base;

	/*
	 * The firmware leaves the 8-byte BAR1 (gpio/irq base info)
	 * unassigned, which makes pci_enable_resources() fail the whole
	 * device with -EINVAL. We don't need BAR1 (bases are hardcoded
	 * per controller above), so clear it so BAR0 enables cleanly.
	 */
	memset(&pdev->resource[1], 0, sizeof(pdev->resource[1]));

	retval = pcim_enable_device(pdev);
	if (retval)
		return retval;

	retval = pcim_iomap_regions(pdev, 1 << 0, pci_name(pdev));
	if (retval) {
		dev_err(&pdev->dev, "I/O memory mapping error\n");
		return retval;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reg_base = pcim_iomap_table(pdev)[0];
	priv->chip.label = dev_name(&pdev->dev);
	priv->chip.parent = &pdev->dev;
	priv->chip.request = intel_gpio_request;
	priv->chip.direction_input = intel_gpio_direction_input;
	priv->chip.direction_output = intel_gpio_direction_output;
	priv->chip.get = intel_gpio_get;
	priv->chip.set = intel_gpio_set;
	priv->chip.base = gpio_base;
	priv->chip.ngpio = ddata->ngpio;
	priv->chip.can_sleep = false;
	priv->pdev = pdev;

	spin_lock_init(&priv->lock);

	girq = &priv->chip.irq;
	girq->chip = &intel_mid_irqchip;
	girq->init_hw = intel_mid_irq_init_hw;
	girq->parent_handler = intel_mid_irq_handler;
	girq->num_parents = 1;
	girq->parents = devm_kcalloc(&pdev->dev, girq->num_parents,
				     sizeof(*girq->parents),
				     GFP_KERNEL);
	if (!girq->parents)
		return -ENOMEM;
	girq->parents[0] = pdev->irq;
	girq->first = irq_base;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_simple_irq;

	pci_set_drvdata(pdev, priv);

	retval = devm_gpiochip_add_data(&pdev->dev, &priv->chip, priv);
	if (retval) {
		dev_err(&pdev->dev, "gpiochip_add error %d\n", retval);
		return retval;
	}

	/*
	 * Cloverview (Acer Iconia W4): raise the always-on module power pads
	 * that U-Boot used to set during board_late_init. They are simple
	 * "leave high" rails (not pulsed), so they belong here rather than in
	 * firmware:
	 *
	 *   - WLAN-enable (global 170 = CORE offset 74): the BCM4330's internal
	 *     regulators stay off until high, so the SDIO function never answers
	 *     CMD5 without it. Runs as soon as the CORE controller registers,
	 *     which PCI discovery guarantees is before the SDIO host (00:04.1)
	 *     scans for the card.
	 *   - bt_reg_on (global 110 = CORE offset 14): the BCM4330 Bluetooth
	 *     module power rail. hci_bcm's vbat/vddio fixed regulators never
	 *     drive a GPIO, so owning this pad here keeps it high before the HSU
	 *     serial port (00:05.0) probes.
*   - gpio_vbenable (global 40 = AON offset 40): gates the SMB347
 *     charger's VBUS detection. Raised when the AON controller
 *     registers, before the charger (I2C2) probes.
 *   - CHRG_OTG (global 132 = CORE offset 36): hardware line telling the
 *     SMB347 to power USB VBUS from its OTG boost. Must be high whenever
 *     the dual-role controller runs as a host, otherwise the micro-USB
 *     port has no 5 V.
 */
	if (boot_cpu_data.x86_vfm == INTEL_ATOM_SALTWELL_TABLET) {
		struct gpio_desc *desc;
		int offset;

		if (id->device == 0x08f7) {			/* Cloverview Core */
			offset = 74;				/* WLAN-enable, gpio 170 */
			desc = gpiochip_request_own_desc(&priv->chip, offset,
							 "WLAN-enable",
							 GPIO_ACTIVE_HIGH,
							 GPIOD_OUT_HIGH);
			if (IS_ERR(desc)) {
				dev_err(&pdev->dev, "WLAN-enable request failed: %ld\n",
					PTR_ERR(desc));
			} else {
				gpiod_set_value(desc, 1);
				dev_info(&pdev->dev, "WLAN-enable asserted (gpio %d)\n",
					 offset);
			}

			offset = 14;				/* bt_reg_on, gpio 110 */
			desc = gpiochip_request_own_desc(&priv->chip, offset,
							 "bt_reg_on",
							 GPIO_ACTIVE_HIGH,
							 GPIOD_OUT_HIGH);
			if (IS_ERR(desc)) {
				dev_err(&pdev->dev, "bt_reg_on request failed: %ld\n",
					PTR_ERR(desc));
			} else {
				gpiod_set_value(desc, 1);
				dev_info(&pdev->dev, "bt_reg_on asserted (gpio %d)\n",
					 offset);
			}

			offset = 36;				/* CHRG_OTG, gpio 132 */
			desc = gpiochip_request_own_desc(&priv->chip, offset,
							 "CHRG_OTG",
							 GPIO_ACTIVE_HIGH,
							 GPIOD_OUT_HIGH);
			if (IS_ERR(desc)) {
				dev_err(&pdev->dev, "CHRG_OTG request failed: %ld\n",
					PTR_ERR(desc));
			} else {
				gpiod_set_value(desc, 1);
				dev_info(&pdev->dev, "CHRG_OTG asserted (gpio %d)\n",
					 offset);
			}
		} else if (id->device == 0x08eb) {			/* Cloverview Aon */
			offset = 40;				/* gpio_vbenable, gpio 40 */
			desc = gpiochip_request_own_desc(&priv->chip, offset,
							 "gpio_vbenable",
							 GPIO_ACTIVE_HIGH,
							 GPIOD_OUT_HIGH);
			if (IS_ERR(desc)) {
				dev_err(&pdev->dev, "gpio_vbenable request failed: %ld\n",
					PTR_ERR(desc));
			} else {
				gpiod_set_value(desc, 1);
				dev_info(&pdev->dev, "gpio_vbenable asserted (gpio %d)\n",
					 offset);
			}
		}
	}

	//pm_runtime_put_noidle(&pdev->dev);
	//pm_runtime_allow(&pdev->dev);

	return 0;
}

static struct pci_driver intel_gpio_driver = {
	.name		= "intel_mid_gpio",
	.id_table	= intel_gpio_ids,
	.probe		= intel_gpio_probe,
	.driver		= {
		//.pm	= &intel_gpio_pm_ops,
	},
};

builtin_pci_driver(intel_gpio_driver);
