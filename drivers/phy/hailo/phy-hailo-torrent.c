// SPDX-License-Identifier: GPL-2.0
/**
 * Wrapper driver for Cadence Torrent Multi-Protocol PHY used in Hailo-15 SoC for Pcie and USB3
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
#include <dt-bindings/phy/phy-hailo-torrent.h>

#define NO_USB_LANE (-1)

#define PCI_PHY_LINK_LANES_CFG_MAX PCI_PHY_LINK_LANES_CFG__4x1__MASTER_LANES_LN0_LN1_LN2_LN4
#define PCI_PMA_PLL_FULL_RATE_CLK_DIVIDER_MAX PCI_PMA_PLL_FULL_RATE_CLK_DIVIDER_8
#define MAX_USB_LANE 3

#define PCIE_PHY_LANE_MODE__PCIE 0
#define PCIE_PHY_LANE_MODE__USB 1

/* pcie config registers */
#define PCIE_CFG 0x9DC
#define  PHY_MODE_LN_0_SHIFT 8
#define  PHY_MODE_LN_0_MASK 0x300
#define  PHY_MODE_LN_1_SHIFT 10
#define  PHY_MODE_LN_1_MASK 0xC00
#define  PHY_MODE_LN_2_SHIFT 12
#define  PHY_MODE_LN_2_MASK 0x3000
#define  PHY_MODE_LN_3_SHIFT 14
#define  PHY_MODE_LN_3_MASK 0xC000
#define PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_0 0x91C
#define  PMA_FULLRT_DIV_LN_0_MASK 0xC00
#define  PMA_FULLRT_DIV_LN_0_SHIFT 10
#define  PHY_LINK_CFG_LN_1_SHIFT 23
#define  PHY_LINK_CFG_LN_1_MASK 0x800000
#define  PMA_FULLRT_DIV_LN_1_MASK 0x3000000
#define  PMA_FULLRT_DIV_LN_1_SHIFT 24
#define PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_1 0x920
#define  PHY_LINK_CFG_LN_2_SHIFT 5
#define  PHY_LINK_CFG_LN_2_MASK 0x20
#define  PMA_FULLRT_DIV_LN_2_MASK 0xC0
#define  PMA_FULLRT_DIV_LN_2_SHIFT 6
#define  PHY_LINK_CFG_LN_3_SHIFT 19
#define  PHY_LINK_CFG_LN_3_MASK 0x80000
#define  PMA_FULLRT_DIV_LN_3_MASK 0x300000
#define  PMA_FULLRT_DIV_LN_3_SHIFT 20
#define PCIE_CONFIG_BYPASS 0x950
#define  PHY_RESET_N_EN_SHIFT 10
#define  PHY_RESET_N_EN_MASK 0x400
#define  PHY_RESET_N_VAL_SHIFT 11
#define  PHY_RESET_N_VAL_MASK 0x800

/* usb config registers */
#define USB_PCIE_PIPE_MUX_CFG 0x5C
#define  USB_PIPE_DMUX_SEL_SHIFT 0
#define  USB_PIPE_DMUX_SEL_MASK 1
#define  USB_RX_PIPE_MUX_SEL_SHIFT 1
#define  USB_RX_PIPE_MUX_SEL_MASK 2
#define  PHY_PIPE_LANES01_MUX_SEL_SHIFT 4
#define  PHY_PIPE_LANES01_MUX_SEL_MASK 0x10
#define  PHY_PIPE_LANES23_MUX_SEL_SHIFT 5
#define  PHY_PIPE_LANES23_MUX_SEL_MASK 0x20

struct hailo_torrent {
	void __iomem *usb_config;
	void __iomem *pcie_config;
	struct clk *usb_pclk;
	struct clk *pcie_pclk;
	struct clk *pcie_aclk;
	int usb_lane;
	u32 lanes_cfg;
	u32 usb_lane_pma_pll_full_rate_divider;
};

static inline u32 hailo_torrent_pcie_config_readl(struct hailo_torrent *data, u32 offset)
{
	return readl(data->pcie_config + offset);
}

static inline void hailo_torrent_pcie_config_writel(struct hailo_torrent *data, u32 offset, u32 value)
{
	writel(value, data->pcie_config + offset);
}

static inline u32 hailo_torrent_usb_config_readl(struct hailo_torrent *data, u32 offset)
{
	return readl(data->usb_config + offset);
}

static inline void hailo_torrent_usb_config_writel(struct hailo_torrent *data, u32 offset, u32 value)
{
	writel(value, data->usb_config + offset);
}

static void pcie_pma_lane_full_rate_clk_divider_cfg(struct hailo_torrent *data, int lane, u32 divider)
{
	u32 offset, mask, shift, value;

	switch(lane) {
	case 0:
		offset = PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_0;
		mask = PMA_FULLRT_DIV_LN_0_MASK;
		shift = PMA_FULLRT_DIV_LN_0_SHIFT;
		break;
	case 1:
		offset = PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_0;
		mask = PMA_FULLRT_DIV_LN_1_MASK;
		shift = PMA_FULLRT_DIV_LN_1_SHIFT;
		break;
	case 2:
		offset = PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_1;
		mask = PMA_FULLRT_DIV_LN_2_MASK;
		shift = PMA_FULLRT_DIV_LN_2_SHIFT;
		break;
	case 3:
		offset = PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_1;
		mask = PMA_FULLRT_DIV_LN_3_MASK;
		shift = PMA_FULLRT_DIV_LN_3_SHIFT;
		break;
	}

	value = hailo_torrent_pcie_config_readl(data, offset);
	value &= ~mask;
	value |= divider << shift;
	hailo_torrent_pcie_config_writel(data, offset, value);
}

static void pcie_phy_link_lanes_cfg(struct hailo_torrent *data)
{
	u32 value;
	u8 cfg_ln_1, cfg_ln_2, cfg_ln_3;

	cfg_ln_1 = (data->lanes_cfg >> 0) & 1;
	cfg_ln_2 = (data->lanes_cfg >> 1) & 1;
	cfg_ln_3 = (data->lanes_cfg >> 2) & 1;

	value = hailo_torrent_pcie_config_readl(data, PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_0);
	value &= ~PHY_LINK_CFG_LN_1_MASK;
	value |= cfg_ln_1 << PHY_LINK_CFG_LN_1_SHIFT;
	hailo_torrent_pcie_config_writel(data, PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_0, value);

	value = hailo_torrent_pcie_config_readl(data, PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_1);
	value &= ~PHY_LINK_CFG_LN_2_MASK;
	value |= cfg_ln_2 << PHY_LINK_CFG_LN_2_SHIFT;
	hailo_torrent_pcie_config_writel(data, PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_1, value);

	value = hailo_torrent_pcie_config_readl(data, PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_1);
	value &= ~PHY_LINK_CFG_LN_3_MASK;
	value |= cfg_ln_3 << PHY_LINK_CFG_LN_3_SHIFT;
	hailo_torrent_pcie_config_writel(data, PHY_CONSTANT_ZERO_VALUE_FOR_DEBUG_1, value);
}

static void pcie_phy_bypass_reset_setup(struct hailo_torrent *data)
{
	u32 value;

	/*
	 *                              __
	 *                             |  \
	 *   PERSTN (0)   ----- AND----|(0)|     PCIE PHY
	 *   MPERST_MASK  -----/       |MUX|---[phy_reset_n]
	 *   BYPASS_VAL   -------------|(1)|
	 *                             |__/
	 *                              |
	 *                          BYPASS_EN
	 */
	value = hailo_torrent_pcie_config_readl(data, PCIE_CONFIG_BYPASS);
	value &= ~PHY_RESET_N_VAL_MASK;
	value |= PHY_RESET_N_EN_MASK;
	hailo_torrent_pcie_config_writel(data, PCIE_CONFIG_BYPASS, value);
}

// usb_lane may be -1 to indicate no USB lane
static void pcie_phy_lanes_mode_cfg(struct hailo_torrent *data)
{
	u32 value;
	u8 lane_0_mode = PCIE_PHY_LANE_MODE__PCIE;
	u8 lane_1_mode = PCIE_PHY_LANE_MODE__PCIE;
	u8 lane_2_mode = PCIE_PHY_LANE_MODE__PCIE;
	u8 lane_3_mode = PCIE_PHY_LANE_MODE__PCIE;

	switch(data->usb_lane) {
	case 0:
		lane_0_mode = PCIE_PHY_LANE_MODE__USB;
		break;
	case 1:
		lane_1_mode = PCIE_PHY_LANE_MODE__USB;
		break;
	case 2:
		lane_2_mode = PCIE_PHY_LANE_MODE__USB;
		break;
	case 3:
		lane_3_mode = PCIE_PHY_LANE_MODE__USB;
		break;
	}

	value = hailo_torrent_pcie_config_readl(data, PCIE_CFG);
	value &= ~PHY_MODE_LN_0_MASK;
	value |= lane_0_mode << PHY_MODE_LN_0_SHIFT;
	value &= ~PHY_MODE_LN_1_MASK;
	value |= lane_1_mode << PHY_MODE_LN_1_SHIFT;
	value &= ~PHY_MODE_LN_2_MASK;
	value |= lane_2_mode << PHY_MODE_LN_2_SHIFT;
	value &= ~PHY_MODE_LN_3_MASK;
	value |= lane_3_mode << PHY_MODE_LN_3_SHIFT;
	hailo_torrent_pcie_config_writel(data, PCIE_CFG, value);
}

static void usb_pcie_pipe_mux_cfg(struct hailo_torrent *data)
{
	u32 value;

	value = hailo_torrent_usb_config_readl(data, USB_PCIE_PIPE_MUX_CFG);
	switch (data->usb_lane) {
	case 3:
		value |= USB_RX_PIPE_MUX_SEL_MASK;
		value |= USB_PIPE_DMUX_SEL_MASK;
		value |= PHY_PIPE_LANES23_MUX_SEL_MASK;
		break;
	case 2:
		value |= USB_RX_PIPE_MUX_SEL_MASK;
		value &= ~USB_PIPE_DMUX_SEL_MASK;
		value |= PHY_PIPE_LANES23_MUX_SEL_MASK;
		break;
	case 1:
		value &= ~USB_RX_PIPE_MUX_SEL_MASK;
		value |= USB_PIPE_DMUX_SEL_MASK;
		value &= ~PHY_PIPE_LANES01_MUX_SEL_MASK;
		break;
	case 0:
		value &= ~USB_RX_PIPE_MUX_SEL_MASK;
		value &= ~USB_PIPE_DMUX_SEL_MASK;
		value &= ~PHY_PIPE_LANES01_MUX_SEL_MASK;
		break;
	}
	hailo_torrent_usb_config_writel(data, USB_PCIE_PIPE_MUX_CFG, value);
}

void hailo_torrent_init(struct hailo_torrent *data)
{
	if (data->usb_lane != NO_USB_LANE) {
		pcie_pma_lane_full_rate_clk_divider_cfg(data, data->usb_lane, data->usb_lane_pma_pll_full_rate_divider);
	}
	pcie_phy_link_lanes_cfg(data);
	pcie_phy_bypass_reset_setup(data);
	pcie_phy_lanes_mode_cfg(data);
	usb_pcie_pipe_mux_cfg(data);
}

static const struct of_dev_auxdata hailo_torrent_auxdata[] = {
	{
		.compatible = "cdns,torrent-phy"
	},
	{},
};

static int hailo_torrent_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct hailo_torrent *data;
	struct resource *usb_res, *pcie_res;
	int ret;

	if (!node)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);

	usb_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "usb-config");
	if (!usb_res) {
		dev_err(dev, "can't get IOMEM usb config resource\n");
		return -ENXIO;
	}
	/* the usb config is shared with the torrent PHY wrapper driver, so therefore
	   we can't use devm_platform_ioremap_resource() */
	data->usb_config = devm_ioremap(&pdev->dev, usb_res->start, resource_size(usb_res));
	if (!data->usb_config) {
		dev_err(dev, "can't map IOMEM usb config resource\n");
		return -ENOMEM;
	}

	pcie_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pcie-config");
	if (!pcie_res) {
		dev_err(dev, "can't get IOMEM pcie config resource\n");
		return -ENXIO;
	}
	/* the pcie config is shared with the pcie endpoint driver, so therefore
	   we can't use devm_platform_ioremap_resource() */
	data->pcie_config = devm_ioremap(&pdev->dev, pcie_res->start, resource_size(pcie_res));
	if (!data->pcie_config) {
		dev_err(dev, "can't map IOMEM pcie config resource\n");
		return -ENOMEM;
	}

	if (of_property_read_u32(pdev->dev.of_node, "lanes-config", &data->lanes_cfg)) {
		dev_err(dev, "lanes-config property not found\n");
		return -EINVAL;
	}
	if (data->lanes_cfg > PCI_PHY_LINK_LANES_CFG_MAX) {
		dev_err(dev, "invalid lanes-config %u", data->lanes_cfg);
		return -EINVAL;
	}

	if (of_property_read_u32(pdev->dev.of_node, "usb-lane-pma-pll-full-rate-divider", &data->usb_lane_pma_pll_full_rate_divider)) {
		dev_err(dev, "usb-lane-pma-pll-full-rate-divider property not found\n");
		return -EINVAL;
	}
	if (data->usb_lane_pma_pll_full_rate_divider > PCI_PMA_PLL_FULL_RATE_CLK_DIVIDER_MAX) {
		dev_err(dev, "invalid usb-lane-pma-pll-full-rate-divider %u", data->usb_lane_pma_pll_full_rate_divider);
		return -EINVAL;
	}

	if (of_property_read_u32(pdev->dev.of_node, "usb-lane", (u32 *)&data->usb_lane)) {
		/* By default assume no usb lane */
		data->usb_lane = NO_USB_LANE;
	}
	if (data->usb_lane > MAX_USB_LANE) {
		dev_err(dev, "invalid usb lane %u", data->usb_lane);
		return -EINVAL;
	}

	data->usb_pclk = devm_clk_get(dev, "usb_pclk");
	if (IS_ERR(data->usb_pclk))
		return PTR_ERR(data->usb_pclk);

	data->pcie_aclk = devm_clk_get(dev, "pcie_aclk");
	if (IS_ERR(data->pcie_aclk))
		return PTR_ERR(data->pcie_aclk);

	data->pcie_pclk = devm_clk_get(dev, "pcie_pclk");
	if (IS_ERR(data->pcie_pclk))
		return PTR_ERR(data->pcie_pclk);

	pm_runtime_get_sync(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = clk_prepare_enable(data->usb_pclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(data->pcie_aclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(data->pcie_pclk);
	if (ret)
		return ret;

	hailo_torrent_init(data);

	clk_disable_unprepare(data->usb_pclk);

	dev_info(dev, "Torrent phy wrapper initialized succesfully (usb in lane %d, lanes config %d, divider %d)",
		data->usb_lane,
		data->lanes_cfg,
		data->usb_lane_pma_pll_full_rate_divider);

	ret = of_platform_populate(node, NULL, hailo_torrent_auxdata, dev);
	if (ret) {
		dev_err(dev, "failed to create children: %d\n", ret);
		goto err;
	}

	return ret;
err:
	clk_disable_unprepare(data->pcie_pclk);
	clk_disable_unprepare(data->pcie_aclk);
	return ret;
}

static int hailo_torrent_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hailo_torrent *data = dev_get_drvdata(dev);

	of_platform_depopulate(dev);
	clk_disable_unprepare(data->pcie_pclk);
	clk_disable_unprepare(data->pcie_aclk);
	pm_runtime_put_sync(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_disable(dev);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

#ifdef CONFIG_PM
static int hailo_torrent_resume(struct device *dev)
{
	int ret;
	struct hailo_torrent *data = dev_get_drvdata(dev);

	ret = clk_prepare_enable(data->pcie_pclk);
	if (ret)
		return ret;

	return clk_prepare_enable(data->pcie_aclk);
}

static int hailo_torrent_suspend(struct device *dev)
{
	struct hailo_torrent *data = dev_get_drvdata(dev);

	clk_disable_unprepare(data->pcie_pclk);
	clk_disable_unprepare(data->pcie_aclk);

	return 0;
}
#endif

static const struct dev_pm_ops hailo_torrent_pm_ops = {
	SET_RUNTIME_PM_OPS(hailo_torrent_suspend, hailo_torrent_resume, NULL)
};

static const struct of_device_id hailo_torrent_of_match[] = {
	{ .compatible = "hailo,torrent-phy", },
	{},
};
MODULE_DEVICE_TABLE(of, hailo_torrent_of_match);

static struct platform_driver hailo_torrent_driver = {
	.probe		= hailo_torrent_probe,
	.remove		= hailo_torrent_remove,
	.driver		= {
		.name	= "phy-hailo-torrent",
		.of_match_table	= hailo_torrent_of_match,
		.pm	= &hailo_torrent_pm_ops,
	},
};
module_platform_driver(hailo_torrent_driver);

MODULE_ALIAS("platform:phy-hailo-torrent");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cadence Torrent Multi-Protocol PHY wrapper driver for Hailo-15 SoC");
