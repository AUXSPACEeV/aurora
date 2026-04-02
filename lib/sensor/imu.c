/**
 * @file imu.c
 * @brief IMU (LSM6DSO32) sensor library implementation.
 *
 * Provides polling and trigger-based interfaces for acceleration and
 * gyroscope data, plus initialization and sampling frequency configuration.
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/imu.h>

#ifndef M_PI
#define M_PI ((float)3.1415926535)
#endif

LOG_MODULE_REGISTER(imu, CONFIG_AURORA_SENSORS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(imu_data_chan,
				 imu_data_t,
				 NULL,
				 NULL,
				 ZBUS_OBSERVERS_EMPTY,
				 ZBUS_MSG_INIT(0));

/**
 * @brief Convert a Zephyr sensor_value to a float.
 *
 * @param val Pointer to the sensor value.
 * @return Floating-point representation.
 */
static inline float out_ev(struct sensor_value *val)
{
	return (val->val1 + (float)val->val2 / 1000000);
}

/* imu_set_sampling_freq – see imu.h */
int imu_set_sampling_freq(const struct device *dev, int sampling_rate_hz)
{
	int ret = 0;
	struct sensor_value odr_attr;

	/* set accel/gyro sampling frequency */
	odr_attr.val1 = (float)sampling_rate_hz;
	odr_attr.val2 = 0;

	ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
						  SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
	if (ret != 0) {
		LOG_ERR("Cannot set sampling frequency for accelerometer.\n");
		return ret;
	}

	ret = sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ,
						  SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
	if (ret != 0) {
		LOG_ERR("Cannot set sampling frequency for gyro.\n");
		return ret;
	}

	return 0;
}

/**
 * @brief Fetch and log accelerometer and gyroscope readings.
 *
 * @param dev Pointer to the IMU device.
 */
static void fetch_and_send(const struct device *dev)
{
	imu_data_t msg;

	if (sensor_sample_fetch(dev) != 0) {
		LOG_ERR("Failed to fetch sensor data\n");
		return;
	}

	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, msg.accel);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ, msg.gyro);

	/* Publish the IMU data to the z-bus channel */
	zbus_chan_pub(&imu_data_chan, &msg, K_NO_WAIT);
}

#if defined(CONFIG_LSM6DSO_TRIGGER)

/**
 * @brief Data-ready trigger callback for the IMU.
 *
 * @param dev  Pointer to the IMU device.
 * @param trig Pointer to the sensor trigger descriptor.
 */
static void trigger_handler(const struct device *dev,
							const struct sensor_trigger *trig)
{
	fetch_and_send(dev);
}

/**
 * @brief Configure the IMU to run in data-ready trigger mode.
 *
 * @param dev Pointer to the IMU device.
 */
static void run_trigger_mode(const struct device *dev)
{
	struct sensor_trigger trig;

	trig.type = SENSOR_TRIG_DATA_READY;
	trig.chan = SENSOR_CHAN_ACCEL_XYZ;

	if (sensor_trigger_set(dev, &trig, trigger_handler) != 0) {
		LOG_ERR("Could not set sensor type and channel\n");
		return;
	}
}

#else
/* imu_poll – see imu.h */
int imu_poll(const struct device *dev)
{
	fetch_and_send(dev);
	return 0;
}
#endif

/* imu_init – see imu.h */
int imu_init(const struct device *dev)
{
	const int imu_hz = CONFIG_IMU_FREQUENCY_VALUE;
	int ret;

	if (!device_is_ready(dev)) {
		LOG_ERR("%s: device not ready.\n", dev->name);
		return -ENODEV;
	}

	ret = imu_set_sampling_freq(dev, imu_hz) != 0;
	if (ret != 0) {
		LOG_WRN("Could not set IMU sampling frequency to %d.0 Hz.\n", imu_hz);
	}

#if defined(CONFIG_LSM6DSO_TRIGGER)
	LOG_DBG("Enableing IMU in trigger mode.\n\n");
	run_trigger_mode(dev);
#endif

	return 0;
}
