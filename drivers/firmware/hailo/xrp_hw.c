// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2017 Cadence Design Systems, Inc.
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 */

#include "xrp_hw.h"

#include "xrp_io.h"
#include "xrp_kernel_dsp_interface.h"
#include "xrp_log.h"

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/align.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/mailbox_client.h>
#include <linux/kernel.h>
#include <linux/irqreturn.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>

void _dsp_config_writel(struct xvp *xvp, size_t offset, u32 value)
{
    if (!__clk_is_enabled(xvp->dsp_config_clock)) {
        dev_err(xvp->dev, "Trying to access dsp config with disabled clock. Aborting\n");
        return;
    }

    iowrite32(value, xvp->dsp_config + offset);
}

u32 _dsp_config_readl(struct xvp *xvp, size_t offset)
{
    if (!__clk_is_enabled(xvp->dsp_config_clock)) {
        dev_err(xvp->dev, "Trying to access dsp config with disabled clock. Aborting\n");
        return -1;
    }

    return ioread32(xvp->dsp_config + offset);
}

void map_dsp_to_physical_address(struct xvp *xvp, uint32_t dsp_address, phys_addr_t physical_address)
{
    return xvp->hw_ops->map_dsp_to_physical_address(xvp, dsp_address, physical_address);
}

static void configure_axi_master_lut(struct xvp *xvp)
{
    // Configure the DSP_AXI_MASTER registers in dsp_config
    // Used to map the DSP's 32 bit address space to 35 bit address space

    dev_dbg(xvp->dev, "Configure AXI lookup table\n");
    
    // DSP ==> Physical

    // map DSP code to 0x80000000
    map_dsp_to_physical_address(xvp, 0x80000000, xvp->mem_ranges.ddr[DDR_MEM_RANGES_CODE].start);

    // F000 0000 - F7FF FFFF ==> 7800 0000 - 7FFF FFFF (mailbox)
    map_dsp_to_physical_address(xvp, DSP_MAILBOX_MAPPED, 0x78000000);

    // fast bus SRAM
    map_dsp_to_physical_address(xvp, DSP_FASTBUS_MEM_MAPPED, 0x60000000);

    // map dsp config
    // dsp config axi is at dsp config + 0x1000, so it will also be mapped
    map_dsp_to_physical_address(xvp, DSP_CONFIG_MAPPED, xvp->dsp_config_phys);

    // map log buffer
    map_dsp_to_physical_address(xvp, xvp->cyclic_log.dsp_paddr, xvp->cyclic_log.paddr);

    // map comm buffer
    map_dsp_to_physical_address(xvp, xvp->comm.dsp_paddr, xvp->comm.paddr);
}

static void disable_wwdt(struct xvp *xvp)
{
    dev_dbg(xvp->dev, "Disable WWDT\n");
    xvp->hw_ops->disable_wwdt(xvp);
}

static void open_interrupts(struct xvp *xvp)
{
    dev_dbg(xvp->dev, "Open DSP interrutps\n");
    xvp->hw_ops->open_interrupts(xvp);
}

static void configure_reset_vector(struct xvp *xvp)
{
    dev_dbg(xvp->dev, "Configure reset vector\n");
    xvp->hw_ops->configure_reset_vector(xvp);
}

static int dsp_config_poweron(struct xvp *xvp)
{
    int ret;

    dev_dbg(xvp->dev, "DSP Config Poweron\n");

    ret = clk_prepare_enable(xvp->dsp_config_clock);
    if (ret) {
        dev_err(xvp->dev, "Error in clock prepare/enable (%d)\n", ret);
        return ret;
    }

    return 0;
}

static void dsp_config_poweroff(struct xvp *xvp)
{
    dev_dbg(xvp->dev, "DSP Config Poweroff\n");

    clk_disable_unprepare(xvp->dsp_config_clock);
}

static int dsp_poweron(struct xvp *xvp)
{
    int ret;
    u32 pll_rate;

    dev_dbg(xvp->dev, "DSP Poweron\n");

    ret = device_property_read_u32(xvp->dev, "clock-frequency", &pll_rate);
    if (ret < 0) {
        dev_err(xvp->dev, "Error in getting pll rate (%d)\n", ret);
        goto exit;
    }

    ret = clk_prepare_enable(xvp->dsp_clock);
    if (ret) {
        dev_err(xvp->dev, "Error in clock enable/prepare (%d)\n", ret);
        goto exit;
    }

    dsp_config_poweroff(xvp);

    ret = clk_set_rate(xvp->dsp_clock, pll_rate);
    if (ret) {
        dev_err(xvp->dev, "Error in clock set rate (%d)\n", ret);
        goto exit;
    }

    ret = clk_prepare_enable(xvp->dsp_config_clock);
    if (ret) {
        dev_err(xvp->dev, "Error in clock prepare/enable (%d)\n", ret);
        goto exit;
    }

    ret = 0;

exit:
    return ret;
}

static int dsp_poweroff(struct xvp *xvp)
{
    int ret;

    dev_dbg(xvp->dev, "DSP Poweroff\n");

    // Linux prints a warning if we try to 
    // unprepare a clock that is already unprepared
    if (__clk_is_enabled(xvp->dsp_clock)) {
        clk_disable_unprepare(xvp->dsp_clock);
    }

    dsp_config_poweroff(xvp);

    ret = reset_control_assert(xvp->dsp_reset);
    if (ret) {
        xvp->state = DSP_STATE_FATAL_ERROR;
        dev_err(xvp->dev, "Failed to assert reset (%d)\n", ret);
        return ret;
    }

    return 0;
}

bool xrp_is_cmd_complete(struct xvp *xvp, struct xrp_comm *xrp_comm)
{
    struct xrp_dsp_cmd __iomem *cmd = xrp_comm->comm;
    u32 flags;

    flags = xrp_comm_read32(&cmd->flags);

    rmb();
    return (flags & (XRP_DSP_CMD_FLAG_REQUEST_VALID |
             XRP_DSP_CMD_FLAG_RESPONSE_VALID)) ==
           (XRP_DSP_CMD_FLAG_REQUEST_VALID |
        XRP_DSP_CMD_FLAG_RESPONSE_VALID);
}

static irqreturn_t xrp_irq_handler(int irq, struct xvp *xvp)
{
    unsigned i, n = 0;

    if (!xvp->comm.addr)
        return IRQ_NONE;

    for (i = 0; i < xvp->n_queues; ++i) {
        if (xrp_is_cmd_complete(xvp, xvp->queue + i)) {
            complete(&xvp->queue[i].completion);
            ++n;
        }
    }
    return n ? IRQ_HANDLED : IRQ_NONE;
}

static void rx_callback(struct mbox_client *cl, void *mssg)
{
    struct xvp *xvp = container_of(cl, struct xvp, mbox_client);
    xrp_irq_handler(0, xvp);
}

static long init_memories(struct platform_device *pdev, struct xvp *xvp)
{
    long ret;
    int i;
    int fw_mem_idx;
    struct resource res;
    struct device_node *node;

    xvp->hw_ops->init_mem_ranges(xvp);

    fw_mem_idx = of_property_match_string(pdev->dev.of_node, "memory-region-names", "dsp-fw");
    node = of_parse_phandle(pdev->dev.of_node, "memory-region", fw_mem_idx);
    if (!node) {
        dev_err(&pdev->dev, "Failed to get specifed device node\n");
        ret = -ENODEV;
        goto exit;
    }

    for (i = 0; i < DDR_MEM_RANGES_COUNT; i++) {
        ret = of_address_to_resource(node, i, &res);
        if (ret) {
            dev_err(&pdev->dev, "No memory address assigned to region %d\n", i);
            goto exit;
        }

        xvp->mem_ranges.ddr[i].start = res.start;
        xvp->mem_ranges.ddr[i].end = res.end + 1;
    }

    xvp->cyclic_log.size = CYCLIC_LOG_MEM_SIZE;
    xvp->cyclic_log.addr = kzalloc(CYCLIC_LOG_MEM_SIZE, GFP_KERNEL);
    if (!xvp->cyclic_log.addr) {
        dev_err(&pdev->dev, "Failed to allocate memory for cyclic log\n");
        ret = -ENOMEM;
        goto exit;
    }

    ret = 0;

exit:
    return ret;
}

static int xrp_create_mbox(struct xvp *xvp)
{
    struct mbox_chan *chan;
    struct mbox_client *mbox_client = &xvp->mbox_client;

    dev_dbg(xvp->dev, "Initializing mailbox\n");
    memset(&xvp->mbox_client, 0, sizeof(xvp->mbox_client));
    mbox_client->dev = xvp->dev;
    mbox_client->rx_callback = rx_callback;
    mbox_client->tx_block = false;
    
    chan = mbox_request_channel(mbox_client, 0);
    if (IS_ERR(chan)) {
        dev_err(xvp->dev, "Mailbox request channel failed: %ld\n", PTR_ERR(chan));
        return PTR_ERR(chan);
    }
    xvp->mbox_chan = chan;

    return 0;
}

static void xrp_destroy_mbox(struct xvp *xvp)
{
    mbox_free_channel(xvp->mbox_chan);
    xvp->mbox_chan = NULL;
}

int xrp_enable_dsp(struct xvp *xvp)
{   
    int ret = reset_control_deassert(xvp->dsp_reset);
    if (ret) {
        dev_err(xvp->dev, "failed to deassert reset (%d)\n", ret);
        goto exit;
    }

    ret = dsp_config_poweron(xvp);
    if (ret) {
        goto exit;
    }

    xrp_halt_dsp(xvp);

    ret = xrp_create_mbox(xvp);
    if (ret < 0) {
        goto exit;
    }

    configure_axi_master_lut(xvp);
    disable_wwdt(xvp);
    open_interrupts(xvp);
    configure_reset_vector(xvp);

    ret = dsp_poweron(xvp);
    if (ret < 0) {
        goto exit;
    }

    ret = 0;

exit:
    if (ret < 0) {
        xvp->state = DSP_STATE_FATAL_ERROR;
    }

    return ret;
}

int xrp_disable_dsp(struct xvp *xvp)
{
    xrp_destroy_mbox(xvp);

    return dsp_poweroff(xvp);
}

void xrp_halt_dsp(struct xvp *xvp)
{
    dev_dbg(xvp->dev, "halt\n");
    xvp->hw_ops->xrp_halt_dsp(xvp);
}

void xrp_release_dsp(struct xvp *xvp)
{
    dev_dbg(xvp->dev, "release\n");
    xvp->hw_ops->xrp_release_dsp(xvp);
}

void xrp_send_device_irq(struct xvp *xvp)
{
    int err = mbox_send_message(xvp->mbox_chan, "");
    if (err < 0) {
        dev_err(xvp->dev, "mbox_send_message error %d\n", err);
    }
}

void xrp_memcpy_tohw(void __iomem *dst, const void *src, size_t sz)
{
    // writing to dsp memory is allowed in 32-bit quantities only
    BUG_ON(!IS_ALIGNED((unsigned long)dst, 4));
    BUG_ON(!IS_ALIGNED((unsigned long)src, 4));
    BUG_ON(!IS_ALIGNED(sz, 4));

    __iowrite32_copy(dst, src, sz / 4);
}

void xrp_memset_hw(void __iomem *to, int c, size_t sz)
{
    // writing to dsp memory is allowed in 32-bit quantities only
    u32 __iomem *dst = to;
    const u32 __iomem *end = dst + (sz / 4);

    BUG_ON(!IS_ALIGNED((unsigned long)dst, 4));
    BUG_ON(!IS_ALIGNED(sz, 4));

    while (dst < end) {
        __raw_writel(c, dst++);
    }
}

long xrp_init_hw_common(struct platform_device *pdev, struct xvp *xvp)
{
    struct resource *mem;
    long ret;

    mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!mem) {
        ret = -ENODEV;
        dev_err(&pdev->dev, "get resource for memory 0 failed: %ld\n", ret);
        goto err;
    }
    xvp->dsp_config_phys = mem->start;
    xvp->dsp_config = devm_ioremap_resource(&pdev->dev, mem);
    if (IS_ERR(xvp->dsp_config)) {
        ret = dev_err_probe(&pdev->dev, PTR_ERR(xvp->dsp_config), "Error in mapping dsp_config\n");
        goto err;
    }

    dev_dbg(&pdev->dev, "Requesting reset object\n");
    xvp->dsp_reset = reset_control_get_exclusive(&pdev->dev, "dsp-reset");
    if (IS_ERR(xvp->dsp_reset)) {
        ret = dev_err_probe(&pdev->dev, PTR_ERR(xvp->dsp_reset), "Error in getting reset object\n");
        goto err;
    }

    dev_dbg(&pdev->dev, "Requesting config clock object\n");
    xvp->dsp_config_clock = devm_clk_get(&pdev->dev, "dsp-config-clock");
    if (IS_ERR(xvp->dsp_config_clock)) {
        ret = dev_err_probe(&pdev->dev, PTR_ERR(xvp->dsp_config_clock), "Error in getting config clock object\n");
        goto err;
    }

    dev_dbg(&pdev->dev, "Requesting dsp clock object\n");
    xvp->dsp_clock = devm_clk_get(&pdev->dev, "dsp-clock");
    if (IS_ERR(xvp->dsp_clock)) {
        ret = dev_err_probe(&pdev->dev, PTR_ERR(xvp->dsp_clock), "Error in getting dsp clock object\n");
        goto err;
    }

    ret = init_memories(pdev, xvp);
    if (ret) {
        dev_err(&pdev->dev, "init_memories() failed: %ld\n", ret);
        goto err;
    }

    ret = 0;

err:
    return ret;
}

bool is_valid_fw_addr(struct xvp *xvp, phys_addr_t addr)
{
    int i;
    int num_ranges = sizeof(xvp->mem_ranges) / sizeof(xvp->mem_ranges.all[0]);
    for (i = 0; i < num_ranges; i++) {
        if (addr >= xvp->mem_ranges.all[i].start &&
            addr < xvp->mem_ranges.all[i].end) {
            return true;
        }
    }

    return false;
}