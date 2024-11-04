// SPDX-License-Identifier: GPL-2.0
/*
 * cdns3-hailo.c - Hailo specific Glue layer for Cadence USB Controller
 *
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include "core.h"
#include "drd.h"

/* usb wrapper config registers */
#define USB_CONFIG 0
#define  MODE_STRAP_MASK 0x3
#define  MODE_STRAP_HOST 0x1
#define USB_INFO_INTR_MASK 0x1C
#define  IRQ_MASK 0x1
#define USB2_PHY_CONFIG 0x4C
#define  ISO_IP2SOC_MASK 0x40

/* usb3 controller xhci registers */
#define XEC_PRE_REG_250NS 0x21e8
#define XEC_PRE_REG_1US 0x21ec
#define XEC_PRE_REG_10US 0x21f0
#define XEC_PRE_REG_100US 0x21f4
#define XEC_PRE_REG_125US 0x21f8
#define XEC_PRE_REG_1MS 0x21fc
#define XEC_PRE_REG_10MS 0x2200
#define XEC_PRE_REG_100MS 0x2204
#define XEC_LPM_PRE_REG_250NS 0x2208
#define XEC_LPM_PRE_REG_1US 0x220c
#define XEC_LPM_PRE_REG_10US 0x2210
#define XEC_LPM_PRE_REG_100US 0x2214
#define XEC_LPM_PRE_REG_125US 0x2218
#define XEC_LPM_PRE_REG_1MS 0x221c
#define XEC_LPM_PRE_REG_10MS 0x2220
#define XEC_LPM_PRE_REG_100MS 0x2224

struct cdns_hailo {
	void __iomem *usb_config;
	struct clk_bulk_data *core_clks;
	int num_core_clks;
	struct clk *pclk;
	bool disconnected_overcurrent;
};

static inline u32 cdns_hailo_readl(struct cdns_hailo *data, u32 offset)
{
	return readl(data->usb_config + offset);
}

static inline void cdns_hailo_writel(struct cdns_hailo *data, u32 offset, u32 value)
{
	writel(value, data->usb_config + offset);
}

static const struct clk_bulk_data hailo_cdns3_core_clks[] = {
	{ .id = "usb_lpm_clk" },
	{ .id = "usb2_refclk" },
	{ .id = "usb_aclk" },
	{ .id = "usb_sof_clk" },
};

void cdns_hailo_init(struct cdns_hailo *data)
{
	u32 value;

    /*  USB mode strap - Default mode to be activated after poweron reset.
            0-neither host nor device.
            1-host.
            2-device (default) */
	value = cdns_hailo_readl(data, USB_CONFIG);
	value &= ~MODE_STRAP_MASK;
	value |= MODE_STRAP_HOST;
	cdns_hailo_writel(data, USB_CONFIG, value);


	/* opening USB info INTR  */
	value = cdns_hailo_readl(data, USB_INFO_INTR_MASK);
	value |= IRQ_MASK;
	cdns_hailo_writel(data, USB_INFO_INTR_MASK, value);

	/* Isolation control pin for all PHY output pins
	* - 0: For isolating IP outputs (default).
	* - 1: For normal operation.
	*/
	value = cdns_hailo_readl(data, USB2_PHY_CONFIG);
	value |= ISO_IP2SOC_MASK;
	cdns_hailo_writel(data, USB2_PHY_CONFIG, value);
}

static int cdns_hailo_xhci_init_quirk(struct usb_hcd *hcd)
{
	struct device *dev = hcd->self.controller;
	struct cdns *cdns = dev_get_drvdata(dev->parent);
	struct cdns_hailo *data = dev_get_drvdata(dev->parent->parent);
	u32 value;

	if (!hcd->regs || !cdns->otg_cdnsp_regs)
		return 0;

    // PRE REG Timers
    writel(0xb, hcd->regs + XEC_PRE_REG_250NS);
    writel(0x2f, hcd->regs + XEC_PRE_REG_1US);
    writel(0x1df, hcd->regs + XEC_PRE_REG_10US);
    writel(0x12bf, hcd->regs + XEC_PRE_REG_100US);
    writel(0x176f, hcd->regs + XEC_PRE_REG_125US);
    writel(0xbb7f, hcd->regs + XEC_PRE_REG_1MS);
    writel(0x752ff, hcd->regs + XEC_PRE_REG_10MS);
    writel(0x493dff, hcd->regs + XEC_PRE_REG_100MS);
    // PRE LMP REG Timers
    writel(0xb, hcd->regs + XEC_LPM_PRE_REG_250NS);
    writel(0x2f, hcd->regs + XEC_LPM_PRE_REG_1US);
    writel(0x1df, hcd->regs + XEC_LPM_PRE_REG_10US);
    writel(0x12bf, hcd->regs + XEC_LPM_PRE_REG_100US);
    writel(0x176f, hcd->regs + XEC_LPM_PRE_REG_125US);
    writel(0xbb7f, hcd->regs + XEC_LPM_PRE_REG_1MS);
    writel(0x752ff, hcd->regs + XEC_LPM_PRE_REG_10MS);
    writel(0x493dff, hcd->regs + XEC_LPM_PRE_REG_100MS);

	/* if overcurrent wire is disconnected, we have to override the overcurrent_n pin */
	if (data->disconnected_overcurrent) {
		value = readl(&cdns->otg_cdnsp_regs->override);
		/* Overcurrent override select, allows SW driver override overcurrent pin as follows:
		* - 0: overcurrent is controlled from external FAULT detector
		* - 1: overcurrent controlled from SFR
		*/
		value |= OVERRIDE_OVERCURRENT_SEL;
		/* SFR overcurrent_n control.
		* - 0: overcurrent_n = 0
		* - 1: overcurrent_n = 1
		* Note: overcurrent active state is low.
		*/
		value |= OVERRIDE_OVERCURRENT_SFR;
		writel(value, &cdns->otg_cdnsp_regs->override);
	}

	return 0;
}

static struct cdns3_platform_data cdns_hailo_pdata = {
	.xhci_init_quirk = cdns_hailo_xhci_init_quirk,
};

static const struct of_dev_auxdata cdns_hailo_auxdata[] = {
	{
		.compatible = "cdns,usb3",
		.platform_data = &cdns_hailo_pdata,
	},
	{},
};

static int cdns_hailo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct cdns_hailo *data;
	struct resource *res;
	int ret;

	if (!node)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "can't get IOMEM resource\n");
		return -ENXIO;
	}

	/* the usb config is shared with the torrent PHY wrapper driver, so therefore
	   we can't use devm_platform_ioremap_resource() */
	data->usb_config = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!data->usb_config) {
		dev_err(dev, "can't map IOMEM resource\n");
		return -ENOMEM;
	}

	data->num_core_clks = ARRAY_SIZE(hailo_cdns3_core_clks);
	data->core_clks = devm_kmemdup(dev, hailo_cdns3_core_clks,
				sizeof(hailo_cdns3_core_clks), GFP_KERNEL);

	if (!data->core_clks)
		return -ENOMEM;

	data->pclk = devm_clk_get(dev, "usb_pclk");
	if (IS_ERR(data->pclk))
		return PTR_ERR(data->pclk);

	data->disconnected_overcurrent = of_property_read_bool(node, "disconnected-overcurrent");

	pm_runtime_get_sync(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = clk_prepare_enable(data->pclk);
	if (ret)
		return ret;

	// note: must be called before the core clocks are enabled
	cdns_hailo_init(data);

	ret = devm_clk_bulk_get(dev, data->num_core_clks, data->core_clks);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(data->num_core_clks, data->core_clks);
	if (ret)
		return ret;

	ret = of_platform_populate(node, NULL, cdns_hailo_auxdata, dev);
	if (ret) {
		dev_err(dev, "failed to create children: %d\n", ret);
		goto err;
	}

	return ret;
err:
	clk_bulk_disable_unprepare(data->num_core_clks, data->core_clks);
	clk_disable_unprepare(data->pclk);
	return ret;
}

static int cdns_hailo_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cdns_hailo *data = dev_get_drvdata(dev);

	of_platform_depopulate(dev);
	clk_bulk_disable_unprepare(data->num_core_clks, data->core_clks);
	clk_disable_unprepare(data->pclk);
	pm_runtime_put_sync(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_disable(dev);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

#ifdef CONFIG_PM
static int cdns_hailo_resume(struct device *dev)
{
	int ret;
	struct cdns_hailo *data = dev_get_drvdata(dev);

	ret = clk_prepare_enable(data->pclk);
	if (ret)
		return ret;

	return clk_bulk_prepare_enable(data->num_core_clks, data->core_clks);
}

static int cdns_hailo_suspend(struct device *dev)
{
	struct cdns_hailo *data = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(data->num_core_clks, data->core_clks);
	clk_disable_unprepare(data->pclk);

	return 0;
}
#endif

static const struct dev_pm_ops cdns_hailo_pm_ops = {
	SET_RUNTIME_PM_OPS(cdns_hailo_suspend, cdns_hailo_resume, NULL)
};

static const struct of_device_id cdns_hailo_of_match[] = {
	{ .compatible = "hailo,usb3", },
	{},
};
MODULE_DEVICE_TABLE(of, cdns_hailo_of_match);

static struct platform_driver cdns_hailo_driver = {
	.probe		= cdns_hailo_probe,
	.remove		= cdns_hailo_remove,
	.driver		= {
		.name	= "cdns3-hailo",
		.of_match_table	= cdns_hailo_of_match,
		.pm	= &cdns_hailo_pm_ops,
	},
};
module_platform_driver(cdns_hailo_driver);

MODULE_ALIAS("platform:cdns3-hailo");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cadence USB3 Hailo Glue Layer");
