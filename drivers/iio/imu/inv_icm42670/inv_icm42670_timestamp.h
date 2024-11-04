/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2019 - 2024 Hailo Technologies Ltd.
 * Based on Copyright (C) 2020 InvenSense:
 * - Linux inv_icm42600
 * - Arduino https://github.com/tdk-invn-oss/motion.arduino.ICM42670P.git
 */

#ifndef INV_ICM42670_TIMESTAMP_H_
#define INV_ICM42670_TIMESTAMP_H_

#include <linux/kernel.h>

struct inv_icm42670_state;

/**
 * struct inv_icm42670_timestamp_interval - timestamps interval
 * @lo:	interval lower bound
 * @up:	interval upper bound
 */
struct inv_icm42670_timestamp_interval {
	int64_t lo;
	int64_t up;
};

/**
 * struct inv_icm42670_timestamp_acc - accumulator for computing an estimation
 * @val:	current estimation of the value, the mean of all values
 * @idx:	current index of the next free place in values table
 * @values:	table of all measured values, use for computing the mean
 */
struct inv_icm42670_timestamp_acc {
	uint32_t val;
	size_t idx;
	uint32_t values[32];
};

/**
 * struct inv_icm42670_timestamp - timestamp management states
 * @it:			interrupts interval timestamps
 * @timestamp:		store last timestamp for computing next data timestamp
 * @mult:		current internal period multiplier
 * @new_mult:		new set internal period multiplier (not yet effective)
 * @period:		measured current period of the sensor
 * @chip_period:	accumulator for computing internal chip period
 */
struct inv_icm42670_timestamp {
	struct inv_icm42670_timestamp_interval it;
	int64_t timestamp;
	uint32_t mult;
	uint32_t new_mult;
	uint32_t period;
	uint32_t need_mclk_cnt;
	struct inv_icm42670_timestamp_acc chip_period;
};

void inv_icm42670_timestamp_init(struct inv_icm42670_timestamp *ts,
				 uint32_t period);

int inv_icm42670_timestamp_setup(struct inv_icm42670_state *st);

int inv_icm42670_timestamp_update_odr(struct inv_icm42670_timestamp *ts,
				      uint32_t period, bool fifo);

void inv_icm42670_timestamp_interrupt(struct inv_icm42670_timestamp *ts,
				      uint32_t fifo_period, size_t fifo_nb,
				      size_t sensor_nb, int64_t timestamp);

static inline int64_t
inv_icm42670_timestamp_pop(struct inv_icm42670_timestamp *ts)
{
	ts->timestamp += ts->period;
	return ts->timestamp;
}

void inv_icm42670_timestamp_apply_odr(struct inv_icm42670_timestamp *ts,
				      uint32_t fifo_period, size_t fifo_nb,
				      unsigned int fifo_no);

static inline void
inv_icm42670_timestamp_reset(struct inv_icm42670_timestamp *ts)
{
	const struct inv_icm42670_timestamp_interval interval_init = {0LL, 0LL};

	ts->it = interval_init;
	ts->timestamp = 0;
}

#endif
