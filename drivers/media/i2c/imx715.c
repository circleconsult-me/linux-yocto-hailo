// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony imx715 sensor driver
 *
 * Copyright (C) 2021 Intel Corporation
 */
#include <asm/unaligned.h>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <linux/kernel.h>
#include "sensor_id.h"


#define DEFAULT_MODE_IDX 0

/* Streaming Mode */
#define IMX715_REG_MODE_SELECT 0x3000
#define IMX715_REG_XMSTA 0x3002
#define IMX715_MODE_STANDBY 0x01
#define IMX715_MODE_STREAMING 0x00

/* Lines per frame */
#define IMX715_REG_LPFR 0x3024 // VMAX

/* Exposure control */
#define IMX715_REG_SHUTTER 0x3050
#define IMX715_EXPOSURE_MIN 1
#define IMX715_EXPOSURE_OFFSET 8
#define IMX715_EXPOSURE_STEP 1
#define IMX715_EXPOSURE_DEFAULT 0x0648

#define IMX715_SDR_4K_VBLANK_MAX (((1 << 20) - 2) - 2160)

/* Analog gain control */
#define IMX715_REG_AGAIN 0x3090
#define IMX715_REG_AGAIN1 0x3092
#define IMX715_REG_AGAIN2 0x3094
#define IMX715_AGAIN_MIN 0
#define IMX715_AGAIN_MAX 240
#define IMX715_AGAIN_STEP 1
#define IMX715_AGAIN_DEFAULT 0

/* Wide Dynamic Range control */
#define IMX715_WDR_MIN 0
#define IMX715_WDR_MAX 1
#define IMX715_WDR_STEP 1
#define IMX715_WDR_DEFAULT 0

/* Group hold register */
#define IMX715_REG_HOLD 0x3001

/* Input clock rate */
#define IMX715_INCLK_RATE 37125000

/* CSI2 HW configuration */
#define IMX715_LINK_FREQ 1782000000
#define IMX715_NUM_DATA_LANES 4

#define IMX715_REG_MIN 0x00
#define IMX715_REG_MAX 0xfffff

#define IMX715_TPG_EN_DUOUT 0x30e0 /* TEST PATTERN ENABLE */
#define IMX715_TPG_PATSEL_DUOUT 0x30e2 /*Patsel mode */
#define IMX715_TPG_COLOR_WIDTH 0x30e4 /*color width */

static int imx715_set_ctrl(struct v4l2_ctrl *ctrl);

/*
 * imx715 test pattern related structure
 */
enum { TEST_PATTERN_DISABLED = 0,
       TEST_PATTERN_ALL_000H,
       TEST_PATTERN_ALL_FFFH,
       TEST_PATTERN_ALL_555H,
       TEST_PATTERN_ALL_AAAH,
       TEST_PATTERN_VSP_5AH, /* VERTICAL STRIPE PATTERN 555H/AAAH */
       TEST_PATTERN_VSP_A5H, /* VERTICAL STRIPE PATTERN AAAH/555H */
       TEST_PATTERN_VSP_05H, /* VERTICAL STRIPE PATTERN 000H/555H */
       TEST_PATTERN_VSP_50H, /* VERTICAL STRIPE PATTERN 555H/000H */
       TEST_PATTERN_VSP_0FH, /* VERTICAL STRIPE PATTERN 000H/FFFH */
       TEST_PATTERN_VSP_F0H, /* VERTICAL STRIPE PATTERN FFFH/000H */
       TEST_PATTERN_H_COLOR_BARS,
       TEST_PATTERN_V_COLOR_BARS,
};

/**
 * enum imx715_test_pattern_menu - imx715 test pattern options
 */
static const char *const imx715_test_pattern_menu[] = {
	"Disabled",
	"All 000h Pattern",
	"All FFFh Pattern",
	"All 555h Pattern",
	"All AAAh Pattern",
	"Vertical Stripe (555h / AAAh)",
	"Vertical Stripe (AAAh / 555h)",
	"Vertical Stripe (000h / 555h)",
	"Vertical Stripe (555h / 000h)",
	"Vertical Stripe (000h / FFFh)",
	"Vertical Stripe (FFFh / 000h)",
	"Horizontal Color Bars",
	"Vertical Color Bars",
};

/* V4l2 subdevice control ops*/
static const struct v4l2_ctrl_ops imx715_ctrl_ops = {
	.s_ctrl = imx715_set_ctrl,
};

/**
 * struct imx715_reg - imx715 sensor register
 * @address: Register address
 * @val: Register value
 */
struct imx715_reg {
	u16 address;
	u8 val;
};

/**
 * struct imx715_reg_list - imx715 sensor register list
 * @num_of_regs: Number of registers in the list
 * @regs: Pointer to register list
 */
struct imx715_reg_list {
	u32 num_of_regs;
	const struct imx715_reg *regs;
};

/**
 * struct imx715_mode - imx715 sensor mode structure
 * @width: Frame width
 * @height: Frame height
 * @code: Format code
 * @hblank: Horizontal blanking in lines
 * @vblank: Vertical blanking in lines
 * @vblank_min: Minimal vertical blanking in lines
 * @vblank_max: Maximum vertical blanking in lines
 * @pclk: Sensor pixel clock
 * @link_freq_idx: Link frequency index
 * @reg_list: Register list for sensor mode
 */
struct imx715_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	u32 link_freq_idx;
	struct imx715_reg_list reg_list;
	struct v4l2_fract frame_interval;
};

/**
 * struct imx715 - imx715 sensor device structure
 * @dev: Pointer to generic device
 * @client: Pointer to i2c client
 * @sd: V4L2 sub-device
 * @pad: Media pad. Only one pad supported
 * @reset_gpio: Sensor reset gpio
 * @inclk: Sensor input clock
 * @ctrl_handler: V4L2 control handler
 * @link_freq_ctrl: Pointer to link frequency control
 * @pclk_ctrl: Pointer to pixel clock control
 * @hblank_ctrl: Pointer to horizontal blanking control
 * @vblank_ctrl: Pointer to vertical blanking control
 * @test_pattern_ctrl: pointer to test pattern control
 * @mode_sel_ctrl: pointer to mode select control
 * @exp_ctrl: Pointer to exposure control
 * @again_ctrl: Pointer to analog gain control
 * @vblank: Vertical blanking in lines
 * @cur_mode: Pointer to current selected sensor mode
 * @mutex: Mutex for serializing sensor controls
 * @streaming: Flag indicating streaming state
 */
struct imx715 {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq_ctrl;
	struct v4l2_ctrl *pclk_ctrl;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct v4l2_ctrl *test_pattern_ctrl;
	struct v4l2_ctrl *mode_sel_ctrl;
	struct {
		struct v4l2_ctrl *exp_ctrl;
		struct v4l2_ctrl *again_ctrl;
	};
	u32 vblank;
	const struct imx715_mode *cur_mode;
	struct mutex mutex;
	bool streaming;
	bool hdr_enabled;
	struct v4l2_subdev_format curr_fmt;
};

static const s64 link_freq[] = {
	IMX715_LINK_FREQ,
};

/* Sensor mode registers -- Tested OK */
static const struct imx715_reg mode_3840x2160_regs[] = {
	{0x3000, 0x01}, // STANDBY					*imx715
	{0x3002, 0x00}, // XMSTA Master mode operation start		*imx715
	/* Hailo Modules are assembled with a 37.125 MHz INCK, modify this section for other oscillators  */
	/* data rate: 1782Mbps/lane */
	{0x3008, 0x7F}, // BC_WAIT_TIME INCK
	{0x300A, 0x5B}, // CP_WAIT_TIME
	{0x3033, 0x04}, // SYS_MODE = 1782Mbps
	{0x3115, 0x00}, // INCKSEL1 
	{0x3116, 0x24}, // INCKSEL2 
	{0x3118, 0xC0}, // INCKSEL3 
	{0x311A, 0xE0}, // INCKSEL4
	{0x311E, 0x24}, // INCKSEL5 
	{0x4004, 0x48}, // TXCLCKES_FREQ
	{0x4005, 0x09}, // TXCLCKES_FREQ
	{0x400C, 0x01}, // INCKSEL6
	{0x4074, 0x00}, // INCKSEL7 for 1782Mbps
	/*Mode : linear: 3840x2160p60 12bits */
	{0x3022, 0x00}, // ADDMODE All-pixel mode
	{0x3024, 0x94}, // VMAX 4500 (for 30FPS)
	{0x3025, 0x11}, // VMAX 
	{0x3028, 0x26}, // HMAX 550
	{0x3029, 0x02}, // HMAX 
	{0x3030, 0x00}, // HREVERSE Normal
	{0x3031, 0x01}, // ADBIT=12Bit
	{0x3032, 0x01}, // MDBIT=12Bit 
	{0x4001, 0x03}, // LaneMode 4Lane
	/* MIPI */
	{0x4018, 0xB7}, // TCLKPOST
	{0x4019, 0x00}, // TCLKPOST
	{0x401A, 0x67}, // TCLKPREPARE
	{0x401B, 0x00}, // TCLKPREPARE
	{0x401C, 0x6F}, // TCLKTRAIL
	{0x401D, 0x00}, // TCLKTRAIL
	{0x401E, 0xDF}, // TCLKZERO
	{0x401F, 0x01}, // TCLKZERO
	{0x4020, 0x6F}, // THSPREPARE
	{0x4021, 0x00}, // THSPREPARE
	{0x4022, 0xCF}, // THSZERO
	{0x4023, 0x00}, // THSZERO
	{0x4024, 0x6F}, // THSTRAIL
	{0x4025, 0x00}, // THSTRAIL
	{0x4026, 0xB7}, // THSEXIT
	{0x4027, 0x00}, // THSEXIT
	{0x4028, 0x5F}, // TLPX
	{0x4029, 0x00}, // TLPX
	/* WINDOW CROPPING MODE */
	{0x301C, 0x04}, // WINMODE cropping mode
	{0x3040, 0x00}, // PIX_HST=00 window cropping mode start position(H)
	{0x3042, 0x00}, // PIX_HWIDTH_LSB=3840	cropping modeH direction(H)
	{0x3043, 0x0f}, // PIX_HWIDTH_MSB
	{0x3044, 0x00}, // PIX_VST=00	window cropping mode start position(V)
	{0x3046, 0xE0}, // PIX_VWIDTH_LSB=2160	cropping modeH direction(V)
	{0x3047, 0x10}, // PIX_VWIDTH_MSB
	{0x30C1, 0x00}, // XVS_DRV XHS_DRV
	{0x32D4, 0x21}, // {Share regs
	{0x32EC, 0xA1}, {0x344C, 0x2B}, {0x344D, 0x01}, {0x344E, 0xED}, {0x344F, 0x01}, 
	{0x3450, 0xF6}, {0x3451, 0x02}, {0x3452, 0x7F}, {0x3453, 0x03}, {0x358A, 0x04}, 
	{0x35A1, 0x02}, {0x35EC, 0x27}, {0x35EE, 0x8D}, {0x35F0, 0x8D}, {0x35F2, 0x29}, 
	{0x36BC, 0x0C}, {0x36CC, 0x53}, {0x36CD, 0x00}, {0x36CE, 0x3C}, {0x36D0, 0x8C},
	{0x36D1, 0x00}, {0x36D2, 0x71}, {0x36D4, 0x3C}, {0x36D6, 0x53}, {0x36D7, 0x00}, 
	{0x36D8, 0x71}, {0x36DA, 0x8C}, {0x36DB, 0x00}, {0x3701, 0x03}, {0x3720, 0x00}, 
	{0x3724, 0x02}, {0x3726, 0x02}, {0x3732, 0x02}, {0x3734, 0x03}, {0x3736, 0x03}, 
	{0x3742, 0x03}, {0x3862, 0xE0}, {0x38CC, 0x30}, {0x38CD, 0x2F}, {0x395C, 0x0C},
	{0x39A4, 0x07}, {0x39A8, 0x32}, {0x39AA, 0x32}, {0x39AC, 0x32}, {0x39AE, 0x32}, 
	{0x39B0, 0x32}, {0x39B2, 0x2F}, {0x39B4, 0x2D}, {0x39B6, 0x28}, {0x39B8, 0x30},
	{0x39BA, 0x30}, {0x39BC, 0x30}, {0x39BE, 0x30}, {0x39C0, 0x30}, {0x39C2, 0x2E},
	{0x39C4, 0x2B}, {0x39C6, 0x25}, {0x3A42, 0xD1}, {0x3A4C, 0x77}, {0x3AE0, 0x02},
	{0x3AEC, 0x0C}, {0x3B00, 0x2E}, {0x3B06, 0x29}, {0x3B98, 0x25}, {0x3B99, 0x21},
	{0x3B9B, 0x13}, {0x3B9C, 0x13}, {0x3B9D, 0x13}, {0x3B9E, 0x13}, {0x3BA1, 0x00},
	{0x3BA2, 0x06}, {0x3BA3, 0x0B}, {0x3BA4, 0x10}, {0x3BA5, 0x14}, {0x3BA6, 0x18},
	{0x3BA7, 0x1A}, {0x3BA8, 0x1A}, {0x3BA9, 0x1A}, {0x3BAC, 0xED}, {0x3BAD, 0x01},
	{0x3BAE, 0xF6}, {0x3BAF, 0x02}, {0x3BB0, 0xA2}, {0x3BB1, 0x03}, {0x3BB2, 0xE0},
	{0x3BB3, 0x03}, {0x3BB4, 0xE0}, {0x3BB5, 0x03}, {0x3BB6, 0xE0}, {0x3BB7, 0x03},
	{0x3BB8, 0xE0}, {0x3BBA, 0xE0}, {0x3BBC, 0xDA}, {0x3BBE, 0x88}, {0x3BC0, 0x44},
	{0x3BC2, 0x7B}, {0x3BC4, 0xA2}, {0x3BC8, 0xBD}, {0x3BCA, 0xBD}, // Share regs}
};

/* Mode wasn't tested */
static const struct imx715_reg mode_1920x1080_sdr_binning_regs[] = {
	{0x3000, 0x01}, // STANDBY					*imx715
	{0x3002, 0x00}, // XMSTA Master mode operation start		*imx715
	/* data rate: 1782Mbps/lane */
	{0x3008, 0x5D}, // BC_WAIT_TIME
	{0x300A, 0x42}, // CP_WAIT_TIME
	{0x3033, 0x04}, // SYS_MODE = 1782Mbps
	{0x3115, 0x00}, // INCKSEL1 
	{0x3116, 0x24}, // INCKSEL2 
	{0x3118, 0xC6}, // INCKSEL3 
	{0x311A, 0xE7}, // INCKSEL4
	{0x311E, 0x23}, // INCKSEL5 
	{0x4004, 0xC0}, // TXCLCKES_FREQ
	{0x4005, 0x06}, // TXCLCKES_FREQ
	{0x400C, 0x01}, // INCKSEL6
	{0x4074, 0x00}, // INCKSEL7 1782Mbps/37.125MHz
	/*Mode : linear: 3840x2160p60 12bits */
	{0x3020, 0x01}, // binning
	{0x3021, 0x01}, // binning
	{0x3022, 0x01}, // binning
	{0x30D9, 0x02}, // binning
	{0x30DA, 0x01}, // binning
	{0x3024, 0xCA}, // VMAX 2250
	{0x3025, 0x08}, // VMAX 
	{0x3028, 0x26}, // HMAX 550
	{0x3029, 0x02}, // HMAX 
	{0x3030, 0x00}, // HREVERSE Normal
	{0x3031, 0x00}, // ADBIT=10Bit
	{0x3032, 0x01}, // MDBIT=12Bit 
	{0x4001, 0x03}, // LaneMode 4Lane
	/* MIPI */
	{0x4018, 0xB7}, // TCLKPOST 
	{0x4019, 0x00}, // TCLKPOST
	{0x401A, 0x67}, // TCLKPREPARE
	{0x401B, 0x00}, // TCLKPREPARE
	{0x401C, 0x6F}, // TCLKTRAIL
	{0x401D, 0x00}, // TCLKTRAIL
	{0x401E, 0xDF}, // TCLKZERO
	{0x401F, 0x01}, // TCLKZERO
	{0x4020, 0x6F}, // THSPREPARE
	{0x4021, 0x00}, // THSPREPARE
	{0x4022, 0xCF}, // THSZERO
	{0x4023, 0x00}, // THSZERO
	{0x4024, 0x6F}, // THSTRAIL
	{0x4025, 0x00}, // THSTRAIL
	{0x4026, 0xB7}, // THSEXIT
	{0x4027, 0x00}, // THSEXIT
	{0x4028, 0x5F}, // TLPX
	{0x4029, 0x00}, // TLPX
	/* WINDOW CROPPING MODE */
	{0x301C, 0x04}, // WINMODE cropping mode
	{0x3040, 0x00}, // PIX_HST=00 window cropping mode start position(H)
	{0x3042, 0x00}, // PIX_HWIDTH_LSB=3840	cropping modeH direction(H)
	{0x3043, 0x0f}, // PIX_HWIDTH_MSB
	{0x3044, 0x00}, // PIX_VST=00	window cropping mode start position(V)
	{0x3046, 0xE0}, // PIX_VWIDTH_LSB=2160	cropping modeH direction(V)
	{0x3047, 0x10}, // PIX_VWIDTH_MSB
	{0x30C1, 0x00}, // XVS_DRV XHS_DRV
	{0x32D4, 0x21}, // {Share regs
	{0x32EC, 0xA1}, {0x344C, 0x2B}, {0x344D, 0x01}, {0x344E, 0xED}, {0x344F, 0x01}, 
	{0x3450, 0xF6}, {0x3451, 0x02}, {0x3452, 0x7F}, {0x3453, 0x03}, {0x358A, 0x04}, 
	{0x35A1, 0x02}, {0x35EC, 0x27}, {0x35EE, 0x8D}, {0x35F0, 0x8D}, {0x35F2, 0x29}, 
	{0x36BC, 0x0C}, {0x36CC, 0x53}, {0x36CD, 0x00}, {0x36CE, 0x3C}, {0x36D0, 0x8C},
	{0x36D1, 0x00}, {0x36D2, 0x71}, {0x36D4, 0x3C}, {0x36D6, 0x53}, {0x36D7, 0x00}, 
	{0x36D8, 0x71}, {0x36DA, 0x8C}, {0x36DB, 0x00}, {0x3701, 0x03}, {0x3720, 0x00}, 
	{0x3724, 0x02}, {0x3726, 0x02}, {0x3732, 0x02}, {0x3734, 0x03}, {0x3736, 0x03}, 
	{0x3742, 0x03}, {0x3862, 0xE0}, {0x38CC, 0x30}, {0x38CD, 0x2F}, {0x395C, 0x0C},
	{0x39A4, 0x07}, {0x39A8, 0x32}, {0x39AA, 0x32}, {0x39AC, 0x32}, {0x39AE, 0x32}, 
	{0x39B0, 0x32}, {0x39B2, 0x2F}, {0x39B4, 0x2D}, {0x39B6, 0x28}, {0x39B8, 0x30},
	{0x39BA, 0x30}, {0x39BC, 0x30}, {0x39BE, 0x30}, {0x39C0, 0x30}, {0x39C2, 0x2E},
	{0x39C4, 0x2B}, {0x39C6, 0x25}, {0x3A42, 0xD1}, {0x3A4C, 0x77}, {0x3AE0, 0x02},
	{0x3AEC, 0x0C}, {0x3B00, 0x2E}, {0x3B06, 0x29}, {0x3B98, 0x25}, {0x3B99, 0x21},
	{0x3B9B, 0x13}, {0x3B9C, 0x13}, {0x3B9D, 0x13}, {0x3B9E, 0x13}, {0x3BA1, 0x00},
	{0x3BA2, 0x06}, {0x3BA3, 0x0B}, {0x3BA4, 0x10}, {0x3BA5, 0x14}, {0x3BA6, 0x18},
	{0x3BA7, 0x1A}, {0x3BA8, 0x1A}, {0x3BA9, 0x1A}, {0x3BAC, 0xED}, {0x3BAD, 0x01},
	{0x3BAE, 0xF6}, {0x3BAF, 0x02}, {0x3BB0, 0xA2}, {0x3BB1, 0x03}, {0x3BB2, 0xE0},
	{0x3BB3, 0x03}, {0x3BB4, 0xE0}, {0x3BB5, 0x03}, {0x3BB6, 0xE0}, {0x3BB7, 0x03},
	{0x3BB8, 0xE0}, {0x3BBA, 0xE0}, {0x3BBC, 0xDA}, {0x3BBE, 0x88}, {0x3BC0, 0x44},
	{0x3BC2, 0x7B}, {0x3BC4, 0xA2}, {0x3BC8, 0xBD}, {0x3BCA, 0xBD}, // Share regs}
};

/* Mode wasn't tested */
static const struct imx715_reg mode_1920x1080_3dol_binning_8fps_regs[] = {
	{0x3002, 0x01}, // XMSTA Master mode operation start		*imx715
	/* data rate: 1782Mbps/lane */
	{0x3008, 0x5D}, // BC_WAIT_TIME
	{0x300A, 0x42}, // CP_WAIT_TIME
	{0x3033, 0x09}, // SYS_MODE = 720Mbps
	{0x3115, 0x00}, // INCKSEL1 
	{0x3116, 0x23}, // INCKSEL2 
	{0x3118, 0xC6}, // INCKSEL3 
	{0x311A, 0xE7}, // INCKSEL4
	{0x311E, 0x23}, // INCKSEL5 
	{0x4004, 0xC0}, // TXCLCKES_FREQ
	{0x4005, 0x06}, // TXCLCKES_FREQ
	{0x400C, 0x01}, // INCKSEL6
	{0x4074, 0x00}, // INCKSEL7 1782Mbps/37.125MHz

	{0x302C, 0x01}, // dol3
	{0x302D, 0x02}, // dol3
	{0x30CF, 0x0F}, // dol3 subsampling

	/*Mode : linear: 3840x2160p60 12bits */
	{0x3020, 0x01}, // binning
	{0x3021, 0x01}, // binning
	{0x3022, 0x01}, // binning
	{0x30D9, 0x02}, // binning
	{0x30DA, 0x01}, // binning
	{0x3024, 0xCA}, // VMAX 2250 
	{0x3025, 0x08}, // VMAX 
	{0x3028, 0x28}, // HMAX 1320
	{0x3029, 0x05}, // HMAX 
	{0x3030, 0x00}, // HREVERSE Normal
	{0x3031, 0x00}, // ADBIT=10Bit
	{0x3032, 0x01}, // MDBIT=12Bit 
	{0x4001, 0x03}, // LaneMode 4Lane
	/* MIPI */
	{0x4018, 0xB7}, // TCLKPOST
	{0x4019, 0x00}, // TCLKPOST
	{0x401A, 0x67}, // TCLKPREPARE
	{0x401B, 0x00}, // TCLKPREPARE
	{0x401C, 0x6F}, // TCLKTRAIL
	{0x401D, 0x00}, // TCLKTRAIL
	{0x401E, 0xDF}, // TCLKZERO
	{0x401F, 0x01}, // TCLKZERO
	{0x4020, 0x6F}, // THSPREPARE
	{0x4021, 0x00}, // THSPREPARE
	{0x4022, 0xCF}, // THSZERO
	{0x4023, 0x00}, // THSZERO
	{0x4024, 0x6F}, // THSTRAIL
	{0x4025, 0x00}, // THSTRAIL
	{0x4026, 0xB7}, // THSEXIT
	{0x4027, 0x00}, // THSEXIT
	{0x4028, 0x5F}, // TLPX
	{0x4029, 0x00}, // TLPX
	/* WINDOW CROPPING MODE */
	{0x301C, 0x04}, // WINMODE cropping mode
	{0x3040, 0x00}, // PIX_HST=00 window cropping mode start position(H)
	{0x3042, 0x00}, // PIX_HWIDTH_LSB=3840	cropping modeH direction(H)
	{0x3043, 0x0f}, // PIX_HWIDTH_MSB
	{0x3044, 0x00}, // PIX_VST=00	window cropping mode start position(V)
	{0x3046, 0xE0}, // PIX_VWIDTH_LSB=2160	cropping modeH direction(V)
	{0x3047, 0x10}, // PIX_VWIDTH_MSB
	{0x30C1, 0x00}, // XVS_DRV XHS_DRV
	{0x32D4, 0x21}, // {Share regs
	{0x32EC, 0xA1}, {0x344C, 0x2B}, {0x344D, 0x01}, {0x344E, 0xED}, {0x344F, 0x01}, 
	{0x3450, 0xF6}, {0x3451, 0x02}, {0x3452, 0x7F}, {0x3453, 0x03}, {0x358A, 0x04}, 
	{0x35A1, 0x02}, {0x35EC, 0x27}, {0x35EE, 0x8D}, {0x35F0, 0x8D}, {0x35F2, 0x29}, 
	{0x36BC, 0x0C}, {0x36CC, 0x53}, {0x36CD, 0x00}, {0x36CE, 0x3C}, {0x36D0, 0x8C},
	{0x36D1, 0x00}, {0x36D2, 0x71}, {0x36D4, 0x3C}, {0x36D6, 0x53}, {0x36D7, 0x00}, 
	{0x36D8, 0x71}, {0x36DA, 0x8C}, {0x36DB, 0x00}, {0x3701, 0x03}, {0x3720, 0x00}, 
	{0x3724, 0x02}, {0x3726, 0x02}, {0x3732, 0x02}, {0x3734, 0x03}, {0x3736, 0x03}, 
	{0x3742, 0x03}, {0x3862, 0xE0}, {0x38CC, 0x30}, {0x38CD, 0x2F}, {0x395C, 0x0C},
	{0x39A4, 0x07}, {0x39A8, 0x32}, {0x39AA, 0x32}, {0x39AC, 0x32}, {0x39AE, 0x32}, 
	{0x39B0, 0x32}, {0x39B2, 0x2F}, {0x39B4, 0x2D}, {0x39B6, 0x28}, {0x39B8, 0x30},
	{0x39BA, 0x30}, {0x39BC, 0x30}, {0x39BE, 0x30}, {0x39C0, 0x30}, {0x39C2, 0x2E},
	{0x39C4, 0x2B}, {0x39C6, 0x25}, {0x3A42, 0xD1}, {0x3A4C, 0x77}, {0x3AE0, 0x02},
	{0x3AEC, 0x0C}, {0x3B00, 0x2E}, {0x3B06, 0x29}, {0x3B98, 0x25}, {0x3B99, 0x21},
	{0x3B9B, 0x13}, {0x3B9C, 0x13}, {0x3B9D, 0x13}, {0x3B9E, 0x13}, {0x3BA1, 0x00},
	{0x3BA2, 0x06}, {0x3BA3, 0x0B}, {0x3BA4, 0x10}, {0x3BA5, 0x14}, {0x3BA6, 0x18},
	{0x3BA7, 0x1A}, {0x3BA8, 0x1A}, {0x3BA9, 0x1A}, {0x3BAC, 0xED}, {0x3BAD, 0x01},
	{0x3BAE, 0xF6}, {0x3BAF, 0x02}, {0x3BB0, 0xA2}, {0x3BB1, 0x03}, {0x3BB2, 0xE0},
	{0x3BB3, 0x03}, {0x3BB4, 0xE0}, {0x3BB5, 0x03}, {0x3BB6, 0xE0}, {0x3BB7, 0x03},
	{0x3BB8, 0xE0}, {0x3BBA, 0xE0}, {0x3BBC, 0xDA}, {0x3BBE, 0x88}, {0x3BC0, 0x44},
	{0x3BC2, 0x7B}, {0x3BC4, 0xA2}, {0x3BC8, 0xBD}, {0x3BCA, 0xBD}, // Share regs}
};

/* Mode wasn't tested */
static const struct imx715_reg mode_1920x1080_3dol_binning_20fps_regs[] = {
	{0x3002, 0x00}, // XMSTA Master mode operation start		*imx715
	/* data rate: 1782Mbps/lane */
	{0x3008, 0x5D}, // BC_WAIT_TIME
	{0x300A, 0x42}, // CP_WAIT_TIME
	{0x3033, 0x04}, // SYS_MODE = 1782Mbps
	{0x3115, 0x00}, // INCKSEL1 
	{0x3116, 0x23}, // INCKSEL2 
	{0x3118, 0xC6}, // INCKSEL3 
	{0x311A, 0xE7}, // INCKSEL4
	{0x311E, 0x23}, // INCKSEL5 
	{0x4004, 0xC0}, // TXCLCKES_FREQ
	{0x4005, 0x06}, // TXCLCKES_FREQ
	{0x400C, 0x01}, // INCKSEL6
	{0x4074, 0x00}, // INCKSEL7 for 1782Mbps
	/*Mode : linear: 3840x2160p60 12bits */
	{0x3020, 0x01}, // binning
	{0x3021, 0x01}, // binning
	{0x3022, 0x01}, // binning
	{0x30D9, 0x02}, // binning
	{0x30DA, 0x01}, // binning
	{0x3024, 0xCA}, // VMAX 2250
	{0x3025, 0x08}, // VMAX 
	{0x3028, 0x26}, // HMAX 550
	{0x3029, 0x02}, // HMAX 
	{0x3030, 0x00}, // HREVERSE Normal
	{0x3031, 0x00}, // ADBIT=10Bit
	{0x3032, 0x01}, // MDBIT=12Bit 
	{0x4001, 0x03}, // LaneMode 4Lane
	/* MIPI */
	{0x4018, 0xB7}, // TCLKPOST for 1782Mbps
	{0x4019, 0x00}, // TCLKPOST
	{0x401A, 0x67}, // TCLKPREPARE
	{0x401B, 0x00}, // TCLKPREPARE
	{0x401C, 0x6F}, // TCLKTRAIL
	{0x401D, 0x00}, // TCLKTRAIL
	{0x401E, 0xDF}, // TCLKZERO
	{0x401F, 0x01}, // TCLKZERO
	{0x4020, 0x6F}, // THSPREPARE
	{0x4021, 0x00}, // THSPREPARE
	{0x4022, 0xCF}, // THSZERO
	{0x4023, 0x00}, // THSZERO
	{0x4024, 0x6F}, // THSTRAIL
	{0x4025, 0x00}, // THSTRAIL
	{0x4026, 0xB7}, // THSEXIT
	{0x4027, 0x00}, // THSEXIT
	{0x4028, 0x5F}, // TLPX
	{0x4029, 0x00}, // TLPX
	/* WINDOW CROPPING MODE */
	{0x301C, 0x04}, // WINMODE cropping mode
	{0x3040, 0x00}, // PIX_HST=00 window cropping mode start position(H)
	{0x3042, 0x00}, // PIX_HWIDTH_LSB=3840	cropping modeH direction(H)
	{0x3043, 0x0f}, // PIX_HWIDTH_MSB
	{0x3044, 0x00}, // PIX_VST=00	window cropping mode start position(V)
	{0x3046, 0xE0}, // PIX_VWIDTH_LSB=2160	cropping modeH direction(V)
	{0x3047, 0x10}, // PIX_VWIDTH_MSB
	{0x30C1, 0x00}, // XVS_DRV XHS_DRV
	{0x32D4, 0x21}, // {Share regs
	{0x32EC, 0xA1}, {0x344C, 0x2B}, {0x344D, 0x01}, {0x344E, 0xED}, {0x344F, 0x01}, 
	{0x3450, 0xF6}, {0x3451, 0x02}, {0x3452, 0x7F}, {0x3453, 0x03}, {0x358A, 0x04}, 
	{0x35A1, 0x02}, {0x35EC, 0x27}, {0x35EE, 0x8D}, {0x35F0, 0x8D}, {0x35F2, 0x29}, 
	{0x36BC, 0x0C}, {0x36CC, 0x53}, {0x36CD, 0x00}, {0x36CE, 0x3C}, {0x36D0, 0x8C},
	{0x36D1, 0x00}, {0x36D2, 0x71}, {0x36D4, 0x3C}, {0x36D6, 0x53}, {0x36D7, 0x00}, 
	{0x36D8, 0x71}, {0x36DA, 0x8C}, {0x36DB, 0x00}, {0x3701, 0x03}, {0x3720, 0x00}, 
	{0x3724, 0x02}, {0x3726, 0x02}, {0x3732, 0x02}, {0x3734, 0x03}, {0x3736, 0x03}, 
	{0x3742, 0x03}, {0x3862, 0xE0}, {0x38CC, 0x30}, {0x38CD, 0x2F}, {0x395C, 0x0C},
	{0x39A4, 0x07}, {0x39A8, 0x32}, {0x39AA, 0x32}, {0x39AC, 0x32}, {0x39AE, 0x32}, 
	{0x39B0, 0x32}, {0x39B2, 0x2F}, {0x39B4, 0x2D}, {0x39B6, 0x28}, {0x39B8, 0x30},
	{0x39BA, 0x30}, {0x39BC, 0x30}, {0x39BE, 0x30}, {0x39C0, 0x30}, {0x39C2, 0x2E},
	{0x39C4, 0x2B}, {0x39C6, 0x25}, {0x3A42, 0xD1}, {0x3A4C, 0x77}, {0x3AE0, 0x02},
	{0x3AEC, 0x0C}, {0x3B00, 0x2E}, {0x3B06, 0x29}, {0x3B98, 0x25}, {0x3B99, 0x21},
	{0x3B9B, 0x13}, {0x3B9C, 0x13}, {0x3B9D, 0x13}, {0x3B9E, 0x13}, {0x3BA1, 0x00},
	{0x3BA2, 0x06}, {0x3BA3, 0x0B}, {0x3BA4, 0x10}, {0x3BA5, 0x14}, {0x3BA6, 0x18},
	{0x3BA7, 0x1A}, {0x3BA8, 0x1A}, {0x3BA9, 0x1A}, {0x3BAC, 0xED}, {0x3BAD, 0x01},
	{0x3BAE, 0xF6}, {0x3BAF, 0x02}, {0x3BB0, 0xA2}, {0x3BB1, 0x03}, {0x3BB2, 0xE0},
	{0x3BB3, 0x03}, {0x3BB4, 0xE0}, {0x3BB5, 0x03}, {0x3BB6, 0xE0}, {0x3BB7, 0x03},
	{0x3BB8, 0xE0}, {0x3BBA, 0xE0}, {0x3BBC, 0xDA}, {0x3BBE, 0x88}, {0x3BC0, 0x44},
	{0x3BC2, 0x7B}, {0x3BC4, 0xA2}, {0x3BC8, 0xBD}, {0x3BCA, 0xBD}, // Share regs}
};

/* Test pattern wasn't attempted */
static const struct imx715_reg imx715_tpg_en_regs[] = {
	//TPG config
	{ 0x3042, 0x00 }, //XSIZE_OVERLAP
	{ 0x30e0, 0x01 }, //TPG_EN_DUOUT
	{ 0x30e4, 0x13 }, //TPG_COLORWIDTH
};

/* Supported sensor mode configurations - Only SDR modes tested were 4K@30 and 4K@60 */
static const struct imx715_mode supported_sdr_modes[] = {
	{
	.width = 3840,
	.height = 2160,
	.hblank = 550,
	.vblank = 2340,
	.vblank_min = 90,
	.vblank_max = IMX715_SDR_4K_VBLANK_MAX,
	.pclk = 594000000,
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SGBRG12_1X12,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_3840x2160_regs),
		.regs = mode_3840x2160_regs,
	},
	.frame_interval = {
		.denominator = 30,
		.numerator = 1,
	},
	},
	{
	.width = 3840,
	.height = 2160,
	.hblank = 550,
	.vblank = 90,
	.vblank_min = 90,
	.vblank_max = 132840,
	.pclk = 594000000,
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SGBRG12_1X12,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_3840x2160_regs),
		.regs = mode_3840x2160_regs,
	},
	.frame_interval = {
		.denominator = 60,
		.numerator = 1,
	},
	},
	{
	.width = 1920,
	.height = 1080,
	.hblank = 550,
	.vblank = 3420,
	.vblank_min = 90,
	.vblank_max = 132840,
	.pclk = 594000000,
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SGBRG12_1X12,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_1920x1080_sdr_binning_regs),
		.regs = mode_1920x1080_sdr_binning_regs,
	},
	.frame_interval = {
		.denominator = 30,
		.numerator = 1,
	},
	},
};

/* Supported sensor mode configurations - Not tested */
static const struct imx715_mode supported_hdr_modes[] = {
	{
	.width = 1920,
	.height = 1080,
	.hblank = 1320,
	.vblank = 1170,
	.vblank_min = 90,
	.vblank_max = 132840,
	.pclk = 594000000,
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SGBRG12_1X12,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_1920x1080_3dol_binning_8fps_regs),
		.regs = mode_1920x1080_3dol_binning_8fps_regs,
	},
	.frame_interval = {
		.denominator = 8,
		.numerator = 1,
	},
	},
	{
	.width = 1920,
	.height = 1080,
	.hblank = 550,
	.vblank = 1170,
	.vblank_min = 90,
	.vblank_max = 132840,
	.pclk = 594000000,
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SGBRG12_1X12,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_1920x1080_3dol_binning_20fps_regs),
		.regs = mode_1920x1080_3dol_binning_20fps_regs,
	},
	.frame_interval = {
		.denominator = 20,
		.numerator = 1,
	},
	},

};

/**
 * to_imx715() - imv715 V4L2 sub-device to imx715 device.
 * @subdev: pointer to imx715 V4L2 sub-device
 *
 * Return: pointer to imx715 device
 */
static inline struct imx715 *to_imx715(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct imx715, sd);
}

/**
 * imx715_read_reg() - Read registers.
 * @imx715: pointer to imx715 device
 * @reg: register address
 * @len: length of bytes to read. Max supported bytes is 4
 * @val: pointer to register value to be filled.
 *
 * Big endian register addresses with little endian values.
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_read_reg(struct imx715 *imx715, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx715->sd);
	struct i2c_msg msgs[2] = { 0 };
	u8 addr_buf[2] = { 0 };
	u8 data_buf[4] = { 0 };
	int ret;

	if (WARN_ON(len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, addr_buf);

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = data_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));

	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = get_unaligned_le32(data_buf);

	return 0;
}

/**
 * imx715_write_reg() - Write register
 * @imx715: pointer to imx715 device
 * @reg: register address
 * @len: length of bytes. Max supported bytes is 4
 * @val: register value
 *
 * Big endian register addresses with little endian values.
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_write_reg(struct imx715 *imx715, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx715->sd);
	u8 buf[6] = { 0 };

	if (WARN_ON(len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_le32(val, buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/**
 * imx715_write_regs() - Write a list of registers
 * @imx715: pointer to imx715 device
 * @regs: list of registers to be written
 * @len: length of registers array
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_write_regs(struct imx715 *imx715,
			     const struct imx715_reg *regs, u32 len)
{
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = imx715_write_reg(imx715, regs[i].address, 1, regs[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * imx715_update_controls() - Update control ranges based on streaming mode
 * @imx715: pointer to imx715 device
 * @mode: pointer to imx715_mode sensor mode
 *
 * Return: 0 if successful, error code otherwise.
 */
/*
static int imx715_update_controls(struct imx715* imx715,
	const struct imx715_mode* mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(imx715->link_freq_ctrl, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(imx715->hblank_ctrl, mode->hblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_modify_range(imx715->vblank_ctrl, mode->vblank_min,
		mode->vblank_max, 1, mode->vblank);
}
*/
/**
 * imx715_update_exp_gain() - Set updated exposure and gain
 * @imx715: pointer to imx715 device
 * @exposure: updated exposure value
 * @gain: updated analog gain value
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_update_exp_gain(struct imx715 *imx715, u32 exposure, u32 gain)
{
	u32 lpfr, shutter;
	int ret;
	
	// vblank = 4500-2160 --> 2340
	lpfr = imx715->vblank + imx715->cur_mode->height; 
	shutter = lpfr - exposure;

	dev_dbg(imx715->dev, "Set long exp %u analog gain %u sh0 %u lpfr %u",
		 exposure, gain, shutter, lpfr);

	ret = imx715_write_reg(imx715, IMX715_REG_HOLD, 1, 1);
	if (ret)
		return ret;

	ret = imx715_write_reg(imx715, IMX715_REG_LPFR, 3, lpfr);
	if (ret)
		goto error_release_group_hold;

	ret = imx715_write_reg(imx715, IMX715_REG_SHUTTER, 3, shutter);
	if (ret)
		goto error_release_group_hold;

	ret = imx715_write_reg(imx715, IMX715_REG_AGAIN, 1, gain);

error_release_group_hold:
	imx715_write_reg(imx715, IMX715_REG_HOLD, 1, 0);

	return ret;
}

/*
 * imx715_set_test_pattern - Function called when setting test pattern
 * @priv: Pointer to device structure
 * @val: Variable for test pattern
 *
 * Set to different test patterns based on input value.
 *
 * Return: 0 on success
*/
static int imx715_set_test_pattern(struct imx715 *imx715, int val)
{
	int ret = 0;

	if (TEST_PATTERN_DISABLED == val)
		ret = imx715_write_reg(imx715, IMX715_TPG_EN_DUOUT, 1, val);
	else {
		ret = imx715_write_reg(imx715, IMX715_TPG_PATSEL_DUOUT, 1,
				       val - 1);
		if (!ret) {
			ret = imx715_write_regs(imx715, imx715_tpg_en_regs,
						ARRAY_SIZE(imx715_tpg_en_regs));
		}
	}
	return ret;
}

/**
 * imx715_set_ctrl() - Set subdevice control
 * @ctrl: pointer to v4l2_ctrl structure
 *
 * Supported controls:
 * - V4L2_CID_VBLANK
 * - V4L2_CID_TEST_PATTERN
 * - cluster controls:
 *   - V4L2_CID_ANALOGUE_GAIN
 *   - V4L2_CID_EXPOSURE
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx715 *imx715 =
		container_of(ctrl->handler, struct imx715, ctrl_handler);
	u32 analog_gain;
	u32 exposure;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		imx715->vblank = imx715->vblank_ctrl->val;

		dev_dbg(imx715->dev, "Received vblank %u, new lpfr %u",
			imx715->vblank,
			imx715->vblank + imx715->cur_mode->height);

		ret = __v4l2_ctrl_modify_range(
			imx715->exp_ctrl, IMX715_EXPOSURE_MIN,
			imx715->vblank + imx715->cur_mode->height -
				IMX715_EXPOSURE_OFFSET,
			1, IMX715_EXPOSURE_DEFAULT);
		break;
	case V4L2_CID_EXPOSURE:

		/* Set controls only if sensor is in power on state */
		if (!pm_runtime_get_if_in_use(imx715->dev))
			return 0;

		exposure = ctrl->val;
		analog_gain = imx715->again_ctrl->val;

		dev_dbg(imx715->dev, "Received exp %u analog gain %u", exposure,
			analog_gain);

		ret = imx715_update_exp_gain(imx715, exposure, analog_gain);

		pm_runtime_put(imx715->dev);

		break;
	case V4L2_CID_TEST_PATTERN:
		if (!pm_runtime_get_if_in_use(imx715->dev))
			return 0;
		ret = imx715_set_test_pattern(imx715, ctrl->val);

		pm_runtime_put(imx715->dev);

		break;
	case V4L2_CID_WIDE_DYNAMIC_RANGE:		
		// Not supported yet
		dev_warn(imx715->dev, "V4L2_CID_WIDE_DYNAMIC_RANGE not suppoterd yet\n");

		if (imx715->streaming) {
			dev_warn(imx715->dev,
				"Cannot set WDR mode while streaming\n");
			return 0;
		}

		imx715->hdr_enabled = ctrl->val;
		dev_dbg(imx715->dev, "hdr enable set to %d\n", imx715->hdr_enabled);
		if (imx715->hdr_enabled)
			imx715->cur_mode = &supported_hdr_modes[0];
		else
			imx715->cur_mode = &supported_sdr_modes[0];
		ret = 0;
		break;
	default:
		dev_err(imx715->dev, "Invalid control %d", ctrl->id);
		ret = -EINVAL;
	}

	return ret;
}

/**
 * imx715_enum_mbus_code() - Enumerate V4L2 sub-device mbus codes
 * @sd: pointer to imx715 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device state
 * @code: V4L2 sub-device code enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx715_mode *supported_modes;
	struct imx715 *imx715 = to_imx715(sd);

	if (code->index > 0)
		return -EINVAL;

	if(imx715->hdr_enabled)
		supported_modes = (struct imx715_mode *)supported_hdr_modes;
	else
		supported_modes = (struct imx715_mode *)supported_sdr_modes;

	mutex_lock(&imx715->mutex);
	code->code = supported_modes[DEFAULT_MODE_IDX].code;
	mutex_unlock(&imx715->mutex);
	return 0;
}

/**
 * imx715_enum_frame_size() - Enumerate V4L2 sub-device frame sizes
 * @sd: pointer to imx715 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device state
 * @fsize: V4L2 sub-device size enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fsize)
{
	struct imx715_mode *supported_modes;
	struct imx715 *imx715 = to_imx715(sd);
	int mode_count, i = 0;
	int min_width = INT_MAX;
	int min_height = INT_MAX;
	int max_width = 0;
	int max_height = 0;

	if (fsize->index > 0)
		return -EINVAL;

	if(imx715->hdr_enabled){
		supported_modes = (struct imx715_mode *)supported_hdr_modes;
		mode_count = sizeof(supported_hdr_modes) / sizeof(struct imx715_mode);
	}
	else {
		supported_modes = (struct imx715_mode *)supported_sdr_modes;
		mode_count = sizeof(supported_sdr_modes) / sizeof(struct imx715_mode);
	}

	mutex_lock(&imx715->mutex);
	for(i = 0; i < mode_count; i++){
		if (fsize->code == supported_modes[i].code) {
			if(supported_modes[i].width > max_width)
				max_width = supported_modes[i].width;
			else if(supported_modes[i].width < min_width)
				min_width = supported_modes[i].width;
			else if(supported_modes[i].height > max_height)
				max_height = supported_modes[i].height;
			else if(supported_modes[i].height < min_height)
				min_height = supported_modes[i].height;
		}
	}

	if (max_width == 0 || max_height == 0 ||
		min_width == INT_MAX || min_height == INT_MAX) {
		pr_debug("%s: Invalid code %d\n", __func__, fsize->code);
		mutex_unlock(&imx715->mutex);
		return -EINVAL;
	}

	fsize->min_width = min_width;
	fsize->max_width = max_width;
	fsize->min_height = min_height;
	fsize->max_height = max_height;
	mutex_unlock(&imx715->mutex);

	return 0;
}

/**
 * imx715_fill_pad_format() - Fill subdevice pad format
 *                            from selected sensor mode
 * @imx715: pointer to imx715 device
 * @mode: pointer to imx715_mode sensor mode
 * @fmt: V4L2 sub-device format need to be filled
 */
static void imx715_fill_pad_format(struct imx715 *imx715,
				   const struct imx715_mode *mode,
				   struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = mode->code;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;
}

/**
 * imx715_get_pad_format() - Get subdevice pad format
 * @sd: pointer to imx715 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device state
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx715 *imx715 = to_imx715(sd);

	mutex_lock(&imx715->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
		fmt->format = *framefmt;
	} else {
		imx715_fill_pad_format(imx715, imx715->cur_mode, fmt);
	}

	mutex_unlock(&imx715->mutex);

	return 0;
}

static int imx715_get_fmt_mode(struct imx715* imx715, struct v4l2_subdev_format* fmt, const struct imx715_mode** mode){
	struct imx715_mode *supported_modes;
	int mode_count = 0;
	int index = 0;

	if(!imx715 || !fmt || !mode)
		return -EINVAL;

	if(imx715->hdr_enabled){
		supported_modes = (struct imx715_mode *)supported_hdr_modes;
		mode_count = sizeof(supported_hdr_modes) / sizeof(struct imx715_mode);
	}
	else {
		supported_modes = (struct imx715_mode *)supported_sdr_modes;
		mode_count = sizeof(supported_sdr_modes) / sizeof(struct imx715_mode);
	}
	
	for (index = 0; index < mode_count; ++index) {
		if(supported_modes[index].width == fmt->format.width && supported_modes[index].height == fmt->format.height){
			*mode = &supported_modes[index];
			return 0;
		}
	}

	return -EINVAL;
}

/**
 * imx715_set_pad_format() - Set subdevice pad format
 * @sd: pointer to imx715 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device state
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx715 *imx715 = to_imx715(sd);
	const struct imx715_mode *mode;
	int ret = 0;
	mutex_lock(&imx715->mutex);
	
	ret = imx715_get_fmt_mode(imx715, fmt, &mode);
	if(ret){
		pr_err("%s - get_fmt failed with %d\n", __func__, ret);
		goto out;
	}

	imx715_fill_pad_format(imx715, mode, fmt);

	// even if which is V4L2_SUBDEV_FORMAT_TRY, update current format for tuning case
	memcpy(&imx715->curr_fmt, fmt, sizeof(struct v4l2_subdev_format));
	imx715->cur_mode = mode;
#ifdef IMX715_UPDATE_CONTROLS_TRY_FMT
		ret = imx715_update_controls(imx687, mode);
#endif

out:
	mutex_unlock(&imx715->mutex);
	return ret;
}

/**
 * imx715_init_pad_cfg() - Initialize sub-device pad configuration
 * @sd: pointer to imx715 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device state
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_init_pad_cfg(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state)
{
	struct imx715_mode *supported_modes;
	struct imx715 *imx715 = to_imx715(sd);
	struct v4l2_subdev_format fmt = { 0 };

	if(imx715->hdr_enabled)
		supported_modes = (struct imx715_mode *)supported_hdr_modes;
	else
		supported_modes = (struct imx715_mode *)supported_sdr_modes;

	fmt.which =
		sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	imx715_fill_pad_format(imx715, &supported_modes[DEFAULT_MODE_IDX],
			       &fmt);

	return imx715_set_pad_format(sd, sd_state, &fmt);
}

/**
 * imx715_start_streaming() - Start sensor stream
 * @imx715: pointer to imx715 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_start_streaming(struct imx715 *imx715)
{
	const struct imx715_reg_list *reg_list;
	int ret;

	/* Write sensor mode registers */
	pr_debug("%s - hdr_enabled: %d\n", __func__, imx715->hdr_enabled);
	reg_list = &imx715->cur_mode->reg_list;
	ret = imx715_write_regs(imx715, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(imx715->dev, "fail to write initial registers");
		return ret;
	}

	/* Setup handler will write actual exposure and gain */
	ret = __v4l2_ctrl_handler_setup(imx715->sd.ctrl_handler);
	if (ret) {
		dev_err(imx715->dev, "fail to setup handler (%d)", ret);
		return ret;
	}

	/* Start streaming */
	ret = imx715_write_reg(imx715, IMX715_REG_MODE_SELECT, 1,
			       IMX715_MODE_STREAMING);
	if (ret) {
		dev_err(imx715->dev, "fail to start streaming");
		return ret;
	}
	/* Start streaming */
	ret = imx715_write_reg(imx715, IMX715_REG_XMSTA, 1, 0);
	if (ret) {
		dev_err(imx715->dev, "fail to start streaming");
		return ret;
	}
	pr_info("imx715: start_streaming successful\n");
	return 0;
}

/**
 * imx715_stop_streaming() - Stop sensor stream
 * @imx715: pointer to imx715 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_stop_streaming(struct imx715 *imx715)
{
	return imx715_write_reg(imx715, IMX715_REG_MODE_SELECT, 1,
				IMX715_MODE_STANDBY);
}

/**
 * imx715_set_stream() - Enable sensor streaming
 * @sd: pointer to imx715 subdevice
 * @enable: set to enable sensor streaming
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx715 *imx715 = to_imx715(sd);
	int ret;

	mutex_lock(&imx715->mutex);

	if (imx715->streaming == enable) {
		mutex_unlock(&imx715->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_resume_and_get(imx715->dev);
		if (ret < 0)
			goto error_unlock;

		ret = imx715_start_streaming(imx715);
		if (ret)
			goto error_power_off;
	} else {
		imx715_stop_streaming(imx715);
		pm_runtime_put(imx715->dev);
	}

	imx715->streaming = enable;

	mutex_unlock(&imx715->mutex);

	return 0;

error_power_off:
	pm_runtime_put(imx715->dev);
error_unlock:
	mutex_unlock(&imx715->mutex);

	return ret;
}

static int
imx715_find_nearest_frame_interval_mode(struct imx715 *imx715,
					struct v4l2_subdev_frame_interval *fi,
					struct imx715_mode const **mode)
{
	struct imx715_mode const* curr_mode;
	struct v4l2_mbus_framefmt* framefmt;
	struct imx715_mode *supported_modes;

	int min_diff = INT_MAX;
	int curr_diff;
	int i, found = 0, mode_count = 0;

	if (!imx715 || !fi) {
		return -EINVAL;
	}

	if(imx715->hdr_enabled){
		supported_modes = (struct imx715_mode *)supported_hdr_modes;
		mode_count = sizeof(supported_hdr_modes) / sizeof(struct imx715_mode);
	}
	else {
		supported_modes = (struct imx715_mode *)supported_sdr_modes;
		mode_count = sizeof(supported_sdr_modes) / sizeof(struct imx715_mode);
	}

	framefmt = &imx715->curr_fmt.format;

	for (i = 0; i < mode_count; ++i) {
		curr_mode = &supported_modes[i];

		if(curr_mode->width != framefmt->width || curr_mode->height != framefmt->height)
			continue;
		found = 1;

		curr_diff = abs(curr_mode->frame_interval.denominator -
				(int)(fi->interval.denominator /
				      fi->interval.numerator));
		if (curr_diff == 0) {
			*mode = curr_mode;
			return 0;
		}
		if (curr_diff < min_diff) {
			min_diff = curr_diff;
			*mode = curr_mode;
		}
	}

	if(!found){
		return -ENOTSUPP;
	}

	return 0;
}

/**
 * imx715_s_frame_interval - Set the frame interval
 * @sd: Pointer to V4L2 Sub device structure
 * @fi: Pointer to V4l2 Sub device frame interval structure
 *
 * This function is used to set the frame intervavl.
 *
 * Return: 0 on success
 */
static int imx715_s_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx715 *imx715 = to_imx715(sd);
	struct imx715_mode const *mode;
	int ret;

	ret = pm_runtime_resume_and_get(imx715->dev);
	if (ret < 0)
		return ret;

	mutex_lock(&imx715->mutex);

	ret = imx715_find_nearest_frame_interval_mode(imx715, fi, &mode);

	if (ret == 0) {
		fi->interval = mode->frame_interval;
		imx715->cur_mode = mode;
		ret = __v4l2_ctrl_s_ctrl(imx715->vblank_ctrl, mode->vblank);
	}

	mutex_unlock(&imx715->mutex);
	pm_runtime_put(imx715->dev);

	return ret;
}

static int imx715_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx715 *imx715 = to_imx715(sd);

	mutex_lock(&imx715->mutex);
	fi->interval = imx715->cur_mode->frame_interval;
	mutex_unlock(&imx715->mutex);

	return 0;
}

static int check_sensor_id(struct imx715 *imx715, u16 id_reg, u8 sensor_id)
{
    int ret;
    u32 id;

    ret = imx715_read_reg(imx715, id_reg, 1, &id);
    if (ret) {
        dev_err(imx715->dev,
            "failed to read sensor id register 0x%x, ret %d\n",
            id_reg, ret);
        return ret;
    }

    if (id != sensor_id) {
        dev_info(imx715->dev,
            "sensor is not connected: (reg %x, expected %x, found %x)",
            id_reg, sensor_id, id);
        return -ENXIO;
    }

    return 0;
}

/**
 * imx715_detect() - Detect imx715 sensor
 * @imx715: pointer to imx715 device
 *
 * Return: 0 if successful, -EIO if sensor id does not match
 */
static int imx715_detect(struct imx715 *imx715)
{
	int ret;
	
	ret = check_sensor_id(imx715, GENERIC_SENSOR_ID_REG, SENSOR_ID_IMX334_IMX715);
	if (ret)
		return ret;
	
	ret = check_sensor_id(imx715, GENERIC_SENSOR_ID_REG2, IMX715_SENSOR_ID_VAL);
	if (ret)
		return ret;
	
	return 0;
}

/**
 * imx715_parse_hw_config() - Parse HW configuration and check if supported
 * @imx715: pointer to imx715 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_parse_hw_config(struct imx715 *imx715)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx715->dev);
	struct v4l2_fwnode_endpoint bus_cfg = { .bus_type =
							V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned long rate;
	int ret;
	int i;

	if (!fwnode)
		return -ENXIO;

	/* Request optional reset pin */
	imx715->reset_gpio =
		devm_gpiod_get_optional(imx715->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(imx715->reset_gpio)) {
		dev_err(imx715->dev, "failed to get reset gpio %ld",
			PTR_ERR(imx715->reset_gpio));
		return PTR_ERR(imx715->reset_gpio);
	}

	/* Get sensor input clock */
	imx715->inclk = devm_clk_get(imx715->dev, NULL);
	if (IS_ERR(imx715->inclk)) {
		dev_err(imx715->dev, "could not get inclk");
		return PTR_ERR(imx715->inclk);
	}

	rate = clk_get_rate(imx715->inclk);
	if (rate != IMX715_INCLK_RATE) {
		dev_warn(imx715->dev, "inclk frequency mismatch - expected: %d actual: %ld\n", IMX715_INCLK_RATE, rate);
		// fixme: For dts generality we're allowing different input clock rates (imx715 modules' freq is different)
		// return -EINVAL;
	}

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX715_NUM_DATA_LANES) {
		dev_err(imx715->dev,
			"number of CSI2 data lanes %d is not supported",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(imx715->dev, "no link frequencies defined");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++)
		if (bus_cfg.link_frequencies[i] == IMX715_LINK_FREQ)
			goto done_endpoint_free;

	ret = -EINVAL;

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

/* V4l2 subdevice ops */
static const struct v4l2_subdev_video_ops imx715_video_ops = {
	.s_stream = imx715_set_stream,
	.s_frame_interval = imx715_s_frame_interval,
	.g_frame_interval = imx715_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx715_pad_ops = {
	.init_cfg = imx715_init_pad_cfg,
	.enum_mbus_code = imx715_enum_mbus_code,
	.enum_frame_size = imx715_enum_frame_size,
	.get_fmt = imx715_get_pad_format,
	.set_fmt = imx715_set_pad_format,
};

static const struct v4l2_subdev_ops imx715_subdev_ops = {
	.video = &imx715_video_ops,
	.pad = &imx715_pad_ops,
};

/**
 * imx715_power_on() - Sensor power on sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx715 *imx715 = to_imx715(sd);
	int ret;

	gpiod_set_value_cansleep(imx715->reset_gpio, 0);
	gpiod_set_value_cansleep(imx715->reset_gpio, 1);

	ret = clk_prepare_enable(imx715->inclk);
	if (ret) {
		dev_err(imx715->dev, "fail to enable inclk");
		goto error_reset;
	}

	usleep_range(18000, 20000);

	return 0;

error_reset:
	gpiod_set_value_cansleep(imx715->reset_gpio, 0);

	return ret;
}

/**
 * imx715_power_off() - Sensor power off sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx715 *imx715 = to_imx715(sd);

	gpiod_set_value_cansleep(imx715->reset_gpio, 0);

	clk_disable_unprepare(imx715->inclk);

	return 0;
}

/**
 * imx715_init_controls() - Initialize sensor subdevice controls
 * @imx715: pointer to imx715 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_init_controls(struct imx715 *imx715)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx715->ctrl_handler;
	const struct imx715_mode *mode = imx715->cur_mode;
	u32 lpfr;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 7);
	if (ret)
		return ret;

	/* Serialize controls with sensor device */
	ctrl_hdlr->lock = &imx715->mutex;

	/* Initialize exposure and gain */
	lpfr = mode->vblank + mode->height;
	imx715->exp_ctrl = v4l2_ctrl_new_std(
		ctrl_hdlr, &imx715_ctrl_ops, V4L2_CID_EXPOSURE,
		IMX715_EXPOSURE_MIN, lpfr - IMX715_EXPOSURE_OFFSET,
		IMX715_EXPOSURE_STEP, IMX715_EXPOSURE_DEFAULT);

	imx715->again_ctrl =
		v4l2_ctrl_new_std(ctrl_hdlr, &imx715_ctrl_ops,
				  V4L2_CID_ANALOGUE_GAIN, IMX715_AGAIN_MIN,
				  IMX715_AGAIN_MAX, IMX715_AGAIN_STEP,
				  IMX715_AGAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &imx715->exp_ctrl);

	imx715->vblank_ctrl =
		v4l2_ctrl_new_std(ctrl_hdlr, &imx715_ctrl_ops, V4L2_CID_VBLANK,
				  mode->vblank_min, mode->vblank_max, 1,
				  mode->vblank);

	imx715->test_pattern_ctrl = v4l2_ctrl_new_std_menu_items(
		ctrl_hdlr, &imx715_ctrl_ops, V4L2_CID_TEST_PATTERN,
		ARRAY_SIZE(imx715_test_pattern_menu) - 1, 0, 0,
		imx715_test_pattern_menu);
	
	imx715->mode_sel_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &imx715_ctrl_ops,
				V4L2_CID_WIDE_DYNAMIC_RANGE, IMX715_WDR_MIN,
				IMX715_WDR_MAX, IMX715_WDR_STEP,
				IMX715_WDR_DEFAULT);

	/* Read only controls */
	imx715->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &imx715_ctrl_ops,
					      V4L2_CID_PIXEL_RATE, mode->pclk,
					      mode->pclk, 1, mode->pclk);

	imx715->link_freq_ctrl = v4l2_ctrl_new_int_menu(
		ctrl_hdlr, &imx715_ctrl_ops, V4L2_CID_LINK_FREQ,
		ARRAY_SIZE(link_freq) - 1, mode->link_freq_idx, link_freq);
	if (imx715->link_freq_ctrl)
		imx715->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx715->hblank_ctrl =
		v4l2_ctrl_new_std(ctrl_hdlr, &imx715_ctrl_ops, V4L2_CID_HBLANK,
				  IMX715_REG_MIN, IMX715_REG_MAX, 1,
				  mode->hblank);
	if (imx715->hblank_ctrl)
		imx715->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (ctrl_hdlr->error) {
		dev_err(imx715->dev, "control init failed: %d",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	imx715->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

/**
 * imx715_probe() - I2C client device binding
 * @client: pointer to i2c client device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_probe(struct i2c_client *client)
{
	struct imx715 *imx715;
	int ret;
	imx715 = devm_kzalloc(&client->dev, sizeof(*imx715), GFP_KERNEL);
	if (!imx715)
		return -ENOMEM;

	imx715->dev = &client->dev;

	dev_info(imx715->dev, "probe started");

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx715->sd, client, &imx715_subdev_ops);

	ret = imx715_parse_hw_config(imx715);
	if (ret) {
		dev_err(imx715->dev, "HW configuration is not supported");
		return ret;
	}

	mutex_init(&imx715->mutex);

	ret = imx715_power_on(imx715->dev);
	if (ret) {
		dev_err(imx715->dev, "failed to power-on the sensor");
		goto error_mutex_destroy;
	}

	/* Check module identity */
	ret = imx715_detect(imx715);
	if (ret) {
		dev_err(imx715->dev, "failed to find sensor: %d", ret);
		goto error_power_off;
	}

	/* Set default mode to max resolution sdr */
	imx715->cur_mode = &supported_sdr_modes[DEFAULT_MODE_IDX];
	imx715->vblank = imx715->cur_mode->vblank;

	ret = imx715_init_controls(imx715);
	if (ret) {
		dev_err(imx715->dev, "failed to init controls: %d", ret);
		goto error_power_off;
	}

	/* Initialize subdev */
	imx715->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx715->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx715->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx715->sd.entity, 1, &imx715->pad);
	if (ret) {
		dev_err(imx715->dev, "failed to init entity pads: %d", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&imx715->sd);
	if (ret < 0) {
		dev_err(imx715->dev, "failed to register async subdev: %d",
			ret);
		goto error_media_entity;
	}

	pm_runtime_set_active(imx715->dev);
	pm_runtime_enable(imx715->dev);
	pm_runtime_idle(imx715->dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx715->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx715->sd.ctrl_handler);
error_power_off:
	imx715_power_off(imx715->dev);
error_mutex_destroy:
	mutex_destroy(&imx715->mutex);

	return ret;
}

/**
 * imx715_remove() - I2C client device unbinding
 * @client: pointer to I2C client device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx715_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx715 *imx715 = to_imx715(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	pm_runtime_suspended(&client->dev);

	mutex_destroy(&imx715->mutex);

	return 0;
}

static const struct dev_pm_ops imx715_pm_ops = { SET_RUNTIME_PM_OPS(
	imx715_power_off, imx715_power_on, NULL) };

static const struct of_device_id imx715_of_match[] = {
	{ .compatible = "sony,imx715" },
	{}
};

MODULE_DEVICE_TABLE(of, imx715_of_match);

static struct i2c_driver imx715_driver = {
	.probe_new = imx715_probe,
	.remove = imx715_remove,
	.driver = {
		.name = "imx715",
		.pm = &imx715_pm_ops,
		.of_match_table = imx715_of_match,
	},
};

module_i2c_driver(imx715_driver);

MODULE_DESCRIPTION("Sony imx715 sensor driver");
MODULE_AUTHOR("Eran Gur, <erang@hailo.ai>");
MODULE_LICENSE("GPL");
