// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony imx675 sensor driver
 *
 * Copyright (C) 2024 Hailo Technologies
 */

#ifndef __IMX675_H__
#define __IMX675_H__

#include <linux/v4l2-controls.h>

#define IMX675_CID_BASE (V4L2_CID_USER_BASE + 0x2000)
#define IMX675_CID_EXPOSURE_SHORT (IMX675_CID_BASE + 1)
#define IMX675_CID_EXPOSURE_VERY_SHORT	(IMX675_CID_BASE + 2)
#define IMX675_CID_ANALOGUE_GAIN_SHORT (IMX675_CID_BASE + 3)
#define IMX675_CID_ANALOGUE_GAIN_VERY_SHORT (IMX675_CID_BASE + 4)

#endif