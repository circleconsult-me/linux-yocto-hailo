/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2019 - 2024 Hailo Technologies Ltd.
 * Based on Copyright (C) 2020 InvenSense:
 * - Linux inv_icm42600
 * - Arduino https://github.com/tdk-invn-oss/motion.arduino.ICM42670P.git
 */

#ifndef INV_ICM42670_TEMP_H_
#define INV_ICM42670_TEMP_H_

#include <linux/iio/iio.h>

#define INV_ICM42670_TEMP_CHAN(_index)			\
	{											\
		.type = IIO_TEMP,						\
		.modified = 1,							\
		.channel2 = IIO_MOD_TEMP_OBJECT,		\
		.info_mask_shared_by_all =				\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
		.info_mask_shared_by_all_available =	\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
		.info_mask_separate =					\
			BIT(IIO_CHAN_INFO_RAW) |			\
			BIT(IIO_CHAN_INFO_OFFSET) |			\
			BIT(IIO_CHAN_INFO_SCALE),			\
		.scan_index = _index,					\
		.scan_type = {							\
			.sign = 's',						\
			.realbits = 16,						\
			.storagebits = 16,					\
		},										\
	}

int inv_icm42670_temp_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask);

#endif
