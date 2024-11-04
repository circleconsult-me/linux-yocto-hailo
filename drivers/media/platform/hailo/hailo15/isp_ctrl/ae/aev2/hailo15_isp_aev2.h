/****************************************************************************
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2023 Vivante Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 *****************************************************************************
 *
 * The GPL License (GPL)
 *
 * Copyright (c) 2014-2023 Vivante Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;
 *
 *****************************************************************************
 *
 * Note: This software is released under dual MIT and GPL licenses. A
 * recipient may use this file under the terms of either the MIT license or
 * GPL License. If you wish to use only one license not the other, you can
 * indicate your decision by deleting one of the above license notices in your
 * version of this file.
 *
 *****************************************************************************/

#ifndef __HAILO15_ISP_AEV2_H__
#define __HAILO15_ISP_AEV2_H__

#define HAILO15_ISP_CID_AE_ENABLE (HAILO15_ISP_CID_AE_BASE + 0x0000)
#define HAILO15_ISP_CID_AE_RESET (HAILO15_ISP_CID_AE_BASE + 0x0001)
#define HAILO15_ISP_CID_AE_SEM_MODE (HAILO15_ISP_CID_AE_BASE + 0x0002)
#define HAILO15_ISP_CID_AE_SETPOINT (HAILO15_ISP_CID_AE_BASE + 0x0003)
#define HAILO15_ISP_CID_AE_DAMPOVER (HAILO15_ISP_CID_AE_BASE + 0x0004)
#define HAILO15_ISP_CID_AE_DAMPUNDER (HAILO15_ISP_CID_AE_BASE + 0x0005)
#define HAILO15_ISP_CID_AE_TOLORANCE (HAILO15_ISP_CID_AE_BASE + 0x0006)
#define HAILO15_ISP_CID_AE_FLICKER_PERIOD (HAILO15_ISP_CID_AE_BASE + 0x0007)
#define HAILO15_ISP_CID_AE_AFPS (HAILO15_ISP_CID_AE_BASE + 0x0008)
#define HAILO15_ISP_CID_AE_GENERATION (HAILO15_ISP_CID_AE_BASE + 0x0009)
#define HAILO15_ISP_CID_AE_HIST (HAILO15_ISP_CID_AE_BASE + 0x000A)
#define HAILO15_ISP_CID_AE_LUMA (HAILO15_ISP_CID_AE_BASE + 0x000B)
#define HAILO15_ISP_CID_AE_OBJECT_REGION (HAILO15_ISP_CID_AE_BASE + 0x000C)
#define HAILO15_ISP_CID_AE_ROI_NUM (HAILO15_ISP_CID_AE_BASE + 0x000D)
#define HAILO15_ISP_CID_AE_ROI_WEIGHT (HAILO15_ISP_CID_AE_BASE + 0x000E)
#define HAILO15_ISP_CID_AE_ROI (HAILO15_ISP_CID_AE_BASE + 0x000F)
#define HAILO15_ISP_CID_AE_GAIN (HAILO15_ISP_CID_AE_BASE + 0x0010)
#define HAILO15_ISP_CID_AE_INTEGRATION_TIME (HAILO15_ISP_CID_AE_BASE + 0x0011)
#define HAILO15_ISP_CID_AE_EXP_STATUS (HAILO15_ISP_CID_AE_BASE + 0x0012)
#define HAILO15_ISP_CID_AE_IRIS (HAILO15_ISP_CID_AE_BASE + 0x0013)
#define HAILO15_ISP_CID_AE_IRIS_LIMITS (HAILO15_ISP_CID_AE_BASE + 0x0014)
#define HAILO15_ISP_CID_AE_FPS (HAILO15_ISP_CID_AE_BASE + 0x0015)
#define HAILO15_ISP_CID_AE_CONVERGED (HAILO15_ISP_CID_AE_BASE + 0x0016)
#define HAILO15_ISP_CID_AE_HIST_MODE (HAILO15_ISP_CID_AE_BASE + 0x0017)
#define HAILO15_ISP_CID_AE_HIST_WINDOW (HAILO15_ISP_CID_AE_BASE + 0x0018)
#define HAILO15_ISP_CID_AE_HIST_WEIGHT (HAILO15_ISP_CID_AE_BASE + 0x0019)
#define HAILO15_ISP_CID_AE_EXP_INPUT (HAILO15_ISP_CID_AE_BASE + 0x001A)
#define HAILO15_ISP_CID_AE_EXP_WINDOW (HAILO15_ISP_CID_AE_BASE + 0x001B)

int hailo15_isp_aev2_ctrl_count(void);
int hailo15_isp_aev2_ctrl_create(struct hailo15_isp_device *isp_dev);

#endif
