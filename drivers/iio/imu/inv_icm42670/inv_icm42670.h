/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2019 - 2024 Hailo Technologies Ltd. Based on:
 * - Linux inv_icm42600
 * - Arduino https://github.com/tdk-invn-oss/motion.arduino.ICM42670P.git
 */

#ifndef INV_ICM42670_H_
#define INV_ICM42670_H_

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/pm.h>
#include <linux/iio/iio.h>

#include "inv_icm42670_defs.h"
#include "inv_icm42670_buffer.h"

enum inv_icm42670_chip {
	INV_CHIP_ICM42670 = 1,
	INV_CHIP_NB,
};

/* serial bus slew rates */
enum inv_icm42670_slew_rate {
	INV_ICM42670_SLEW_RATE_20_60NS,
	INV_ICM42670_SLEW_RATE_12_36NS,
	INV_ICM42670_SLEW_RATE_6_19NS,
	INV_ICM42670_SLEW_RATE_4_14NS,
	INV_ICM42670_SLEW_RATE_2_8NS,
	INV_ICM42670_SLEW_RATE_INF_2NS,
};

/* Same as INV_ICM42670_PWR_MGMT0_GYRO_MODE_t and INV_ICM42670_PWR_MGMT0_ACCEL_MODE_t*/
enum inv_icm42670_sensor_mode {
	INV_ICM42670_SENSOR_MODE_OFF,
	INV_ICM42670_SENSOR_MODE_STANDBY,  // Gyro only
	INV_ICM42670_SENSOR_MODE_LOW_POWER,
	INV_ICM42670_SENSOR_MODE_LOW_NOISE,
	INV_ICM42670_SENSOR_MODE_NB,
};

enum inv_icm42670_gyro_fs {
	INV_ICM42670_GYRO_FS_2000DPS,
	INV_ICM42670_GYRO_FS_1000DPS,
	INV_ICM42670_GYRO_FS_500DPS,
	INV_ICM42670_GYRO_FS_250DPS,
	INV_ICM42670_GYRO_FS_NB,
};

/* accelerometer fullscale values:  same as INV_ICM42670_ACCEL_CONFIG0_FS_SEL_t */
enum inv_icm42670_accel_fs {
	INV_ICM42670_ACCEL_FS_16G,
	INV_ICM42670_ACCEL_FS_8G,
	INV_ICM42670_ACCEL_FS_4G,
	INV_ICM42670_ACCEL_FS_2G,
	INV_ICM42670_ACCEL_FS_NB,
};

/* ODR suffixed by LN or LP are Low-Noise or Low-Power mode only 
 * Same as INV_ICM42670_GYRO_CONFIG0_ODR_t INV_ICM42670_ACCEL_CONFIG0_ODR_t
 */
enum inv_icm42670_odr {
	INV_ICM42670_ODR_NB        = 0x10,
	INV_ICM42670_ODR_1_5625_HZ = 0xF,
	INV_ICM42670_ODR_3_125_HZ  = 0xE,
	INV_ICM42670_ODR_6_25_HZ   = 0xD,
	INV_ICM42670_ODR_12_5_HZ   = 0xC,
	INV_ICM42670_ODR_25_HZ     = 0xB,
	INV_ICM42670_ODR_50_HZ     = 0xA,
	INV_ICM42670_ODR_100_HZ    = 0x9,
	INV_ICM42670_ODR_200_HZ    = 0x8,
	INV_ICM42670_ODR_400_HZ    = 0x7,
	INV_ICM42670_ODR_800_HZ    = 0x6,
	INV_ICM42670_ODR_1600_HZ   = 0x5,
};

enum inv_icm42670_filter_bw {
	INV_ICM42670_FILT_BW_16        = 0x7,
	INV_ICM42670_FILT_BW_25        = 0x6,
	INV_ICM42670_FILT_BW_34        = 0x5,
	INV_ICM42670_FILT_BW_53        = 0x4,
	INV_ICM42670_FILT_BW_73        = 0x3,
	INV_ICM42670_FILT_BW_121       = 0x2,
	INV_ICM42670_FILT_BW_180       = 0x1,
	INV_ICM42670_FILT_BW_NO_FILTER = 0x0,
};

struct inv_icm42670_sensor_conf {
	int mode;
	int fs;
	int odr;
	int filter;
};
#define INV_ICM42670_SENSOR_CONF_INIT		{-1, -1, -1, -1}

struct inv_icm42670_conf {
	struct inv_icm42670_sensor_conf gyro;
	struct inv_icm42670_sensor_conf accel;
	bool temp_en;
	//bool fifo_highres_enabled;
};

struct inv_icm42670_suspended {
	enum inv_icm42670_sensor_mode gyro;
	enum inv_icm42670_sensor_mode accel;
	bool temp;
	//bool fifo_highres_enabled;
};

/**
 *  struct inv_icm42670_state - driver state variables
 *  @lock:		lock for serializing multiple registers access.
 *  @chip:		chip identifier.
 *  @name:		chip name.
 *  @map:		regmap pointer.
 *  @vdd_supply:	VDD voltage regulator for the chip.
 *  @vddio_supply:	I/O voltage regulator for the chip.
 *  @orientation:	sensor chip orientation relative to main hardware.
 *  @conf:		chip sensors configurations.
 *  @suspended:		suspended sensors configuration.
 *  @indio_gyro:	gyroscope IIO device.
 *  @indio_accel:	accelerometer IIO device.
 *  @buffer:		data transfer buffer aligned for DMA.
 *  @fifo:		FIFO management structure.
 *  @timestamp:		interrupt timestamps.
 */
struct inv_icm42670_state {
	struct mutex lock;
	enum inv_icm42670_chip chip;
	const char *name;
	struct regmap *map;
	struct regulator *vdd_supply;
	struct regulator *vddio_supply;
	struct iio_mount_matrix orientation;
	struct inv_icm42670_conf conf;
	struct inv_icm42670_suspended suspended;
	struct iio_dev *indio_gyro;
	struct iio_dev *indio_accel;
	uint8_t buffer[2] ____cacheline_aligned;
	struct inv_icm42670_fifo fifo;
	struct {
		int64_t gyro;
		int64_t accel;
	} timestamp;
	/** Internal counter for MCLK requests. */
	uint8_t need_mclk_cnt;
};

/* Sleep times required by the driver */
#define INV_ICM42670_POWER_UP_TIME_MS		100
#define INV_ICM42670_RESET_TIME_MS		1
#define INV_ICM42670_ACCEL_STARTUP_TIME_MS	20
#define INV_ICM42670_GYRO_STARTUP_TIME_MS	60
#define INV_ICM42670_GYRO_STOP_TIME_MS		150
#define INV_ICM42670_TEMP_STARTUP_TIME_MS	14
#define INV_ICM42670_SUSPEND_DELAY_MS		2000

typedef int (*inv_icm42670_bus_setup)(struct inv_icm42670_state *);

extern const struct regmap_config inv_icm42670_regmap_config;
extern const struct dev_pm_ops inv_icm42670_pm_ops;

const struct iio_mount_matrix *
inv_icm42670_get_mount_matrix(const struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan);

uint32_t inv_icm42670_odr_to_period(enum inv_icm42670_odr odr);

int inv_icm42670_set_accel_conf(struct inv_icm42670_state *st,
				struct inv_icm42670_sensor_conf *conf,
				unsigned int *sleep_ms);

int inv_icm42670_set_gyro_conf(struct inv_icm42670_state *st,
			       struct inv_icm42670_sensor_conf *conf,
			       unsigned int *sleep_ms);

int inv_icm42670_set_temp_conf(struct inv_icm42670_state *st, bool enable,
			       unsigned int *sleep_ms);

int inv_icm42670_debugfs_reg(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval);

int inv_icm42670_core_probe(struct regmap *regmap, int chip, int irq,
			    inv_icm42670_bus_setup bus_setup);

struct iio_dev *inv_icm42670_gyro_init(struct inv_icm42670_state *st);

int inv_icm42670_gyro_parse_fifo(struct iio_dev *indio_dev);

struct iio_dev *inv_icm42670_accel_init(struct inv_icm42670_state *st);

int inv_icm42670_accel_parse_fifo(struct iio_dev *indio_dev);

#endif
