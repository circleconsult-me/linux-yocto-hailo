// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019 - 2024 Hailo Technologies Ltd.
 * Based on Copyright (C) 2020 InvenSense:
 * - Linux inv_icm42600
 * - Arduino https://github.com/tdk-invn-oss/motion.arduino.ICM42670P.git
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>

#include "inv_icm42670.h"
#include "inv_icm42670_buffer.h"
#include "inv_icm42670_timestamp.h"

const struct regmap_config inv_icm42670_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x5FFF,
};
EXPORT_SYMBOL_GPL(inv_icm42670_regmap_config);

struct inv_icm42670_hw {
	uint8_t whoami;
	const char *name;
	const struct inv_icm42670_conf *conf;
};

/* chip initial default configuration */
static const struct inv_icm42670_conf inv_icm42670_default_conf = {
	.gyro = {
		.mode = INV_ICM42670_SENSOR_MODE_OFF,
		.fs = INV_ICM42670_GYRO_FS_2000DPS,
		.odr = INV_ICM42670_ODR_200_HZ,
		.filter = INV_ICM42670_FILT_BW_180,
	},
	.accel = {
		.mode = INV_ICM42670_SENSOR_MODE_OFF,
		.fs = INV_ICM42670_ACCEL_FS_16G,
		.odr = INV_ICM42670_ODR_200_HZ,
		.filter = INV_ICM42670_FILT_BW_180,
	},
	.temp_en = true, /* Always on */
	// TODO add support for High resolution packet
	//.fifo_highres_enabled = false,
};

static const struct inv_icm42670_hw inv_icm42670_hw[INV_CHIP_NB] = {
	[INV_CHIP_ICM42670] = {
		.whoami = INV_ICM42670_WHO_AM_I_ICM42670,
		.name = "icm42670",
		.conf = &inv_icm42670_default_conf,
	},
};

const struct iio_mount_matrix *
inv_icm42670_get_mount_matrix(const struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan)
{
	const struct inv_icm42670_state *st = iio_device_get_drvdata(indio_dev);

	return &st->orientation;
}

uint32_t inv_icm42670_odr_to_period(enum inv_icm42670_odr odr)
{
	static uint32_t odr_periods[INV_ICM42670_ODR_NB] = {
		[INV_ICM42670_ODR_1_5625_HZ] = (1000000000 / 15625),
		[INV_ICM42670_ODR_3_125_HZ] = (1000000000 / 3125),
		[INV_ICM42670_ODR_6_25_HZ] = (1000000000 / 625),
		[INV_ICM42670_ODR_12_5_HZ] = (1000000000 / 125),
		[INV_ICM42670_ODR_25_HZ] = (1000000000 / 25),
		[INV_ICM42670_ODR_50_HZ] = (1000000000 / 50),
		[INV_ICM42670_ODR_100_HZ] = (1000000000 / 100),
		[INV_ICM42670_ODR_200_HZ] = (1000000000 / 200),
		[INV_ICM42670_ODR_400_HZ] = (1000000000 / 400),
		[INV_ICM42670_ODR_800_HZ] = (1000000000 / 800),
		[INV_ICM42670_ODR_1600_HZ] = (1000000000 / 1600),
	};

	return odr_periods[odr];
}

static int inv_icm42670_set_pwr_mgmt0(struct inv_icm42670_state *st,
				      enum inv_icm42670_sensor_mode gyro,
				      enum inv_icm42670_sensor_mode accel,
				      unsigned int *sleep_ms)
{
	enum inv_icm42670_sensor_mode oldgyro = st->conf.gyro.mode;
	enum inv_icm42670_sensor_mode oldaccel = st->conf.accel.mode;
	unsigned int sleepval;
	unsigned int val;
	int ret;

	/* if nothing changed, exit */
	if (gyro == oldgyro && accel == oldaccel) // && temp == oldtemp)
		return 0;

	val = INV_ICM42670_PWR_MGMT0_GYRO_MODE(gyro) |
	      INV_ICM42670_PWR_MGMT0_ACCEL_MODE(accel);

	ret = regmap_write(st->map, INV_ICM42670_PWR_MGMT0, val);
	if (ret)
		return ret;

	st->conf.gyro.mode = gyro;
	st->conf.accel.mode = accel;
	st->conf.temp_en = true;

	/* compute required wait time for sensors to stabilize */
	sleepval = 0;
	/* temperature stabilization time */
	sleepval = INV_ICM42670_TEMP_STARTUP_TIME_MS;
	//	}
	/* accel startup time */
	if (accel != oldaccel && oldaccel == INV_ICM42670_SENSOR_MODE_OFF) {
		/* block any register write for at least 200 µs */
		usleep_range(200, 300);
		if (sleepval < INV_ICM42670_ACCEL_STARTUP_TIME_MS)
			sleepval = INV_ICM42670_ACCEL_STARTUP_TIME_MS;
	}
	if (gyro != oldgyro) {
		/* gyro startup time */
		if (oldgyro == INV_ICM42670_SENSOR_MODE_OFF) {
			/* block any register write for at least 200 µs */
			usleep_range(200, 300);
			if (sleepval < INV_ICM42670_GYRO_STARTUP_TIME_MS)
				sleepval = INV_ICM42670_GYRO_STARTUP_TIME_MS;
			/* gyro stop time */
		} else if (gyro == INV_ICM42670_SENSOR_MODE_OFF) {
			if (sleepval < INV_ICM42670_GYRO_STOP_TIME_MS)
				sleepval = INV_ICM42670_GYRO_STOP_TIME_MS;
		}
	}

	/* deferred sleep value if sleep pointer is provided or direct sleep */
	if (sleep_ms)
		*sleep_ms = sleepval;
	else if (sleepval)
		msleep(sleepval);

	return 0;
}

int inv_icm42670_set_accel_conf(struct inv_icm42670_state *st,
				struct inv_icm42670_sensor_conf *conf,
				unsigned int *sleep_ms)
{
	struct inv_icm42670_sensor_conf *oldconf = &st->conf.accel;
	unsigned int val;
	int ret;

	/* Sanitize missing values with current values */
	if (conf->mode < 0)
		conf->mode = oldconf->mode;
	if (conf->fs < 0)
		conf->fs = oldconf->fs;
	if (conf->odr < 0)
		conf->odr = oldconf->odr;
	if (conf->filter < 0)
		conf->filter = oldconf->filter;

	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = INV_ICM42670_ACCEL_CONFIG0_ACCEL_UI_FS(conf->fs) |
		      INV_ICM42670_ACCEL_CONFIG0_ACCEL_ODR(conf->odr);
		ret = regmap_write(st->map, INV_ICM42670_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* set ACCEL_CONFIG1 register (accel filter) */
	if (conf->filter != oldconf->filter) {
		val = INV_ICM42670_ACCEL_CONFIG1_ACCEL_UI_FILT_BW(conf->filter);
		ret = regmap_write(st->map, INV_ICM42670_ACCEL_CONFIG1, val);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* set PWR_MGMT0 register (accel sensor mode) */
	return inv_icm42670_set_pwr_mgmt0(st, st->conf.gyro.mode, conf->mode,
					  sleep_ms);
}

int inv_icm42670_set_gyro_conf(struct inv_icm42670_state *st,
			       struct inv_icm42670_sensor_conf *conf,
			       unsigned int *sleep_ms)
{
	struct inv_icm42670_sensor_conf *oldconf = &st->conf.gyro;
	unsigned int val;
	int ret;

	/* sanitize missing values with current values */
	if (conf->mode < 0)
		conf->mode = oldconf->mode;
	if (conf->fs < 0)
		conf->fs = oldconf->fs;
	if (conf->odr < 0)
		conf->odr = oldconf->odr;
	if (conf->filter < 0)
		conf->filter = oldconf->filter;

	/* set GYRO_CONFIG0 register (gyro fullscale & odr) */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = INV_ICM42670_GYRO_CONFIG0_GYRO_UI_FS(conf->fs) |
		      INV_ICM42670_GYRO_CONFIG0_GYRO_ODR(conf->odr);
		ret = regmap_write(st->map, INV_ICM42670_GYRO_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* set GYRO_CONFIG1 register (gyro filter) */
	if (conf->filter != oldconf->filter) {
		val = INV_ICM42670_GYRO_CONFIG1_GYRO_UI_FILT_BW(conf->filter);
		ret = regmap_write(st->map, INV_ICM42670_GYRO_CONFIG1, val);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* set PWR_MGMT0 register (gyro sensor mode) */
	return inv_icm42670_set_pwr_mgmt0(st, conf->mode, st->conf.accel.mode,
					  sleep_ms);

	return 0;
}

int inv_icm42670_set_temp_conf(struct inv_icm42670_state *st, bool enable,
			       unsigned int *sleep_ms)
{
	return inv_icm42670_set_pwr_mgmt0(st, st->conf.gyro.mode,
					  st->conf.accel.mode, sleep_ms);
}

int inv_icm42670_debugfs_reg(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct inv_icm42670_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	mutex_lock(&st->lock);

	if (readval)
		ret = regmap_read(st->map, reg, readval);
	else
		ret = regmap_write(st->map, reg, writeval);

	mutex_unlock(&st->lock);

	return ret;
}

static int inv_icm42670_set_conf(struct inv_icm42670_state *st,
				 const struct inv_icm42670_conf *conf)
{
	unsigned int val;
	int ret;

	/* set PWR_MGMT0 register (gyro & accel sensor mode, temp enabled) */
	val = INV_ICM42670_PWR_MGMT0_GYRO_MODE(conf->gyro.mode) |
	      INV_ICM42670_PWR_MGMT0_ACCEL_MODE(conf->accel.mode);
	ret = regmap_write(st->map, INV_ICM42670_PWR_MGMT0, val);
	if (ret)
		return ret;

	/* set GYRO_CONFIG0 register (gyro fullscale & odr) */
	val = INV_ICM42670_GYRO_CONFIG0_GYRO_UI_FS(conf->gyro.fs) |
	      INV_ICM42670_GYRO_CONFIG0_GYRO_ODR(conf->gyro.odr);
	ret = regmap_write(st->map, INV_ICM42670_GYRO_CONFIG0, val);
	if (ret)
		return ret;

	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	val = INV_ICM42670_ACCEL_CONFIG0_ACCEL_UI_FS(conf->accel.fs) |
	      INV_ICM42670_ACCEL_CONFIG0_ACCEL_ODR(conf->accel.odr);
	ret = regmap_write(st->map, INV_ICM42670_ACCEL_CONFIG0, val);
	if (ret)
		return ret;

	/* set GYRO_CONFIG1 register */
	val = INV_ICM42670_GYRO_CONFIG1_GYRO_UI_FILT_BW(conf->gyro.filter);
	ret = regmap_write(st->map, INV_ICM42670_GYRO_CONFIG1, val);
	if (ret)
		return ret;

	/* set ACCEL_CONFIG1 register */
	val = INV_ICM42670_ACCEL_CONFIG1_ACCEL_UI_FILT_BW(conf->accel.filter);
	ret = regmap_write(st->map, INV_ICM42670_ACCEL_CONFIG1, val);
	if (ret)
		return ret;

	/* update internal conf */
	st->conf = *conf;

	return 0;
}

/**
 *  inv_icm42670_setup() - check and setup chip
 *  @st:	driver internal state
 *  @bus_setup:	callback for setting up bus specific registers
 *
 *  Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42670_setup(struct inv_icm42670_state *st,
			      inv_icm42670_bus_setup bus_setup)
{
	const struct inv_icm42670_hw *hw = &inv_icm42670_hw[st->chip];
	const struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/* check chip self-identification value */
	ret = regmap_read(st->map, INV_ICM42670_WHO_AM_I, &val);
	if (ret)
		return ret;
	if (val != hw->whoami) {
		dev_err(dev, "invalid whoami %#02x expected %#02x (%s)\n", val,
			hw->whoami, hw->name);
		return -ENODEV;
	}
	st->name = hw->name;

	/* reset to make sure previous state are not there */
	ret = regmap_write(
		st->map, INV_ICM42670_SIGNAL_PATH_RESET,
		INV_ICM42670_SIGNAL_PATH_RESET_SOFT_RESET_DEVICE_CONFIG_EN);
	if (ret)
		return ret;
	msleep(INV_ICM42670_RESET_TIME_MS);

	ret = regmap_read(st->map, INV_ICM42670_INT_STATUS, &val);
	if (ret)
		return ret;
	if (!(val & INV_ICM42670_INT_STATUS_RESET_DONE_INT_MASK)) {
		dev_err(dev, "reset error, reset done bit not set\n");
		return -ENODEV;
	}

	/* set chip bus configuration */
	ret = bus_setup(st);
	if (ret)
		return ret;

	/* sensor data in big-endian (default) */
	ret = regmap_update_bits(
		st->map, INV_ICM42670_INTF_CONFIG0,
		INV_ICM42670_INTF_CONFIG0_SENSOR_DATA_ENDIAN_MASK,
		INV_ICM42670_INTF_CONFIG0_DATA_BIG_ENDIAN);
	if (ret)
		return ret;

	return inv_icm42670_set_conf(st, hw->conf);
}

static irqreturn_t inv_icm42670_irq_timestamp(int irq, void *_data)
{
	struct inv_icm42670_state *st = _data;

	st->timestamp.gyro = iio_get_time_ns(st->indio_gyro);
	st->timestamp.accel = iio_get_time_ns(st->indio_accel);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t inv_icm42670_irq_handler(int irq, void *_data)
{
	struct inv_icm42670_state *st = _data;
	struct device *dev = regmap_get_device(st->map);
	unsigned int status;
	int ret;

	mutex_lock(&st->lock);

	ret = regmap_read(st->map, INV_ICM42670_INT_STATUS, &status);
	if (ret)
		goto out_unlock;

	/* FIFO full */
	if (status & INV_ICM42670_INT_STATUS_FIFO_FULL_INT_MASK)
		dev_warn(dev, "FIFO full data lost!\n");

	/* FIFO threshold reached */
	if (status & INV_ICM42670_INT_STATUS_FIFO_THS_INT_MASK) {
		ret = inv_icm42670_buffer_fifo_read(st, 0);
		if (ret) {
			dev_err(dev, "FIFO read error %d\n", ret);
			goto out_unlock;
		}
		ret = inv_icm42670_buffer_fifo_parse(st);
		if (ret)
			dev_err(dev, "FIFO parsing error %d\n", ret);
	}

out_unlock:
	mutex_unlock(&st->lock);
	return IRQ_HANDLED;
}

/**
 * inv_icm42670_irq_init() - initialize int pin and interrupt handler
 * @st:		driver internal state
 * @irq:	irq number
 * @irq_type:	irq trigger type
 * @open_drain:	true if irq is open drain, false for push-pull
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42670_irq_init(struct inv_icm42670_state *st, int irq,
				 int irq_type, bool open_drain)
{
	struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/* configure INT1 interrupt: default is active low on edge */
	switch (irq_type) {
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		val = INV_ICM42670_INT_CONFIG_INT1_POLARITY_HIGH;
		break;
	default:
		val = INV_ICM42670_INT_CONFIG_INT1_POLARITY_LOW;
		break;
	}

	switch (irq_type) {
	case IRQF_TRIGGER_LOW:
	case IRQF_TRIGGER_HIGH:
		val |= INV_ICM42670_INT_CONFIG_INT1_MODE_LATCHED;
		break;
	default:
		break;
	}

	if (!open_drain)
		val |= INV_ICM42670_INT_CONFIG_INT1_DRIVE_CIRCUIT_PP;

	ret = regmap_write(st->map, INV_ICM42670_INT_CONFIG, val);
	if (ret)
		return ret;

	/* Deassert async reset for proper INT pin operation (cf datasheet) */
	ret = regmap_update_bits(st->map, INV_ICM42670_INT_CONFIG1_MREG1,
				 INV_ICM42670_INT_CONFIG1_INT_ASYNC_RESET_MASK,
				 0);
	if (ret)
		return ret;

	return devm_request_threaded_irq(dev, irq, inv_icm42670_irq_timestamp,
					 inv_icm42670_irq_handler, irq_type,
					 "inv_icm42670", st);
}

static int inv_icm42670_enable_regulator_vddio(struct inv_icm42670_state *st)
{
	int ret;

	ret = regulator_enable(st->vddio_supply);
	if (ret)
		return ret;

	/* wait a little for supply ramp */
	usleep_range(3000, 4000);

	return 0;
}

static void inv_icm42670_disable_vdd_reg(void *_data)
{
	struct inv_icm42670_state *st = _data;
	const struct device *dev = regmap_get_device(st->map);
	int ret;

	ret = regulator_disable(st->vdd_supply);
	if (ret)
		dev_err(dev, "failed to disable vdd error %d\n", ret);
}

static void inv_icm42670_disable_vddio_reg(void *_data)
{
	struct inv_icm42670_state *st = _data;
	const struct device *dev = regmap_get_device(st->map);
	int ret;

	ret = regulator_disable(st->vddio_supply);
	if (ret)
		dev_err(dev, "failed to disable vddio error %d\n", ret);
}

static void inv_icm42670_disable_pm(void *_data)
{
	struct device *dev = _data;

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

int inv_icm42670_core_probe(struct regmap *regmap, int chip, int irq,
			    inv_icm42670_bus_setup bus_setup)
{
	struct device *dev = regmap_get_device(regmap);
	struct inv_icm42670_state *st;
	struct irq_data *irq_desc;
	int irq_type;
	bool open_drain;
	int ret;

	if (chip < 0 || chip >= INV_CHIP_NB) {
		dev_err(dev, "invalid chip = %d\n", chip);
		return -ENODEV;
	}

	/* get irq properties, set trigger falling by default */
	irq_desc = irq_get_irq_data(irq);
	if (!irq_desc) {
		dev_err(dev, "could not find IRQ %d\n", irq);
		return -EINVAL;
	}

	irq_type = irqd_get_trigger_type(irq_desc);
	if (!irq_type)
		irq_type = IRQF_TRIGGER_FALLING;

	open_drain = device_property_read_bool(dev, "drive-open-drain");

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	dev_set_drvdata(dev, st);
	mutex_init(&st->lock);
	st->chip = chip;
	st->map = regmap;

	ret = iio_read_mount_matrix(dev, &st->orientation);
	if (ret) {
		dev_err(dev, "failed to retrieve mounting matrix %d\n", ret);
		return ret;
	}

	st->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(st->vdd_supply))
		return PTR_ERR(st->vdd_supply);

	st->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(st->vddio_supply))
		return PTR_ERR(st->vddio_supply);

	ret = regulator_enable(st->vdd_supply);
	if (ret)
		return ret;
	msleep(INV_ICM42670_POWER_UP_TIME_MS);

	ret = devm_add_action_or_reset(dev, inv_icm42670_disable_vdd_reg, st);
	if (ret)
		return ret;

	ret = inv_icm42670_enable_regulator_vddio(st);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, inv_icm42670_disable_vddio_reg, st);
	if (ret)
		return ret;

	/* setup chip registers */
	ret = inv_icm42670_setup(st, bus_setup);
	if (ret)
		return ret;

	ret = inv_icm42670_timestamp_setup(st);
	if (ret)
		return ret;

	ret = inv_icm42670_buffer_init(st);
	if (ret)
		return ret;

	st->indio_gyro = inv_icm42670_gyro_init(st);
	if (IS_ERR(st->indio_gyro))
		return PTR_ERR(st->indio_gyro);

	st->indio_accel = inv_icm42670_accel_init(st);
	if (IS_ERR(st->indio_accel))
		return PTR_ERR(st->indio_accel);

	ret = inv_icm42670_irq_init(st, irq, irq_type, open_drain);
	if (ret)
		return ret;

	/* setup runtime power management */
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, INV_ICM42670_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put(dev);

	return devm_add_action_or_reset(dev, inv_icm42670_disable_pm, dev);
}
EXPORT_SYMBOL_GPL(inv_icm42670_core_probe);

/*
 * Suspend saves sensors state and turns everything off.
 * Check first if runtime suspend has not already done the job.
 */
static int __maybe_unused inv_icm42670_suspend(struct device *dev)
{
	struct inv_icm42670_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	st->suspended.gyro = st->conf.gyro.mode;
	st->suspended.accel = st->conf.accel.mode;
	st->suspended.temp = st->conf.temp_en;
	if (pm_runtime_suspended(dev)) {
		ret = 0;
		goto out_unlock;
	}

	/* disable FIFO data streaming */
	if (st->fifo.on) {
		ret = regmap_write(st->map, INV_ICM42670_FIFO_CONFIG1,
				   INV_ICM42670_FIFO_CONFIG1_FIFO_BYPASS_ON);
		if (ret)
			goto out_unlock;
	}

	ret = inv_icm42670_set_pwr_mgmt0(st, INV_ICM42670_SENSOR_MODE_OFF,
					 INV_ICM42670_SENSOR_MODE_OFF, NULL);
	if (ret)
		goto out_unlock;

	regulator_disable(st->vddio_supply);

out_unlock:
	mutex_unlock(&st->lock);
	return ret;
}

/*
 * System resume gets the system back on and restores the sensors state.
 * Manually put runtime power management in system active state.
 */
static int __maybe_unused inv_icm42670_resume(struct device *dev)
{
	struct inv_icm42670_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	ret = inv_icm42670_enable_regulator_vddio(st);
	if (ret)
		goto out_unlock;

	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/* restore sensors state */
	ret = inv_icm42670_set_pwr_mgmt0(st, st->suspended.gyro,
					 st->suspended.accel, NULL);
	if (ret)
		goto out_unlock;

	/* restore FIFO data streaming */
	if (st->fifo.on)
		ret = regmap_write(st->map, INV_ICM42670_FIFO_CONFIG1,
				   INV_ICM42670_FIFO_CONFIG1_FIFO_MODE_STREAM);

out_unlock:
	mutex_unlock(&st->lock);
	return ret;
}

/* Runtime suspend will turn off sensors that are enabled by iio devices. */
static int __maybe_unused inv_icm42670_runtime_suspend(struct device *dev)
{
	struct inv_icm42670_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	/* disable all sensors */
	ret = inv_icm42670_set_pwr_mgmt0(st, INV_ICM42670_SENSOR_MODE_OFF,
					 INV_ICM42670_SENSOR_MODE_OFF, NULL);
	if (ret)
		goto error_unlock;

	regulator_disable(st->vddio_supply);

error_unlock:
	mutex_unlock(&st->lock);
	return ret;
}

/* Sensors are enabled by iio devices, no need to turn them back on here. */
static int __maybe_unused inv_icm42670_runtime_resume(struct device *dev)
{
	struct inv_icm42670_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	ret = inv_icm42670_enable_regulator_vddio(st);

	mutex_unlock(&st->lock);
	return ret;
}

const struct dev_pm_ops inv_icm42670_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(inv_icm42670_suspend, inv_icm42670_resume)
		SET_RUNTIME_PM_OPS(inv_icm42670_runtime_suspend,
				   inv_icm42670_runtime_resume, NULL)
};
EXPORT_SYMBOL_GPL(inv_icm42670_pm_ops);

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-426xx device driver");
MODULE_LICENSE("GPL");
