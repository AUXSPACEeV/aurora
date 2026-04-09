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

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/imu.h>

#ifndef M_PI
#define M_PI ((double)3.1415926535)
#endif

LOG_MODULE_REGISTER(imu, CONFIG_AURORA_SENSORS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(imu_data_chan,
		 struct imu_data,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/**
 * @brief Convert a Zephyr sensor_value to a double.
 *
 * @param val Pointer to the sensor value.
 * @return Floating-point representation.
 */
static inline double out_ev(const struct sensor_value *val)
{
	return (val->val1 + (double)val->val2 / 1000000);
}

/**
 * @brief Fetch and log accelerometer and gyroscope readings.
 *
 * @param dev Pointer to the IMU device.
 *
 * @return 0 on success, -errno on failure.
 */
static int fetch_and_send(const struct device *dev)
{
	struct imu_data msg;
	int ret;

	ret = sensor_sample_fetch(dev);
	if ( ret != 0) {
		LOG_ERR("Failed to fetch sensor data");
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, msg.accel);
	if (ret != 0) {
		LOG_ERR("Failed to get accelerometer data");
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ, msg.gyro);
	if (ret != 0) {
		LOG_ERR("Failed to get gyroscope data");
		return ret;
	}

	/* Publish the IMU data to the z-bus channel */
	ret = zbus_chan_pub(&imu_data_chan, &msg, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR("Failed to publish IMU data");
	}
	return ret;
}

#if defined(CONFIG_IMU_TRIGGER)
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
		LOG_ERR("Could not set sensor type and channel");
		return;
	}
}

#else
/* imu_poll – see imu.h */
int imu_poll(const struct device *dev)
{
	if (dev == NULL)
		return -EINVAL;

	return fetch_and_send(dev);
}
#endif /* CONFIG_IMU_TRIGGER */

/* imu_init – see imu.h */
int imu_init(const struct device *dev)
{
	if (!device_is_ready(dev)) {
		LOG_ERR("%s: device not ready.", dev->name);
		return -ENODEV;
	}

#if defined(CONFIG_IMU_TRIGGER)
	LOG_DBG("Enabling IMU in trigger mode.");
	run_trigger_mode(dev);
#endif

	return 0;
}

/* imu_sensor_value_to_acceleration – see imu.h */
int imu_sensor_value_to_acceleration(const struct imu_data *data, double *acc_out)
{
	if (data == NULL || acc_out == NULL)
		return -EINVAL;

	double x = out_ev(&data->accel[0]);
	double y = out_ev(&data->accel[1]);
	double z = out_ev(&data->accel[2]);
	*acc_out = sqrt(x*x + y*y + z*z);
	return 0;
}

/* imu_sensor_value_to_orientation – see imu.h */
int imu_sensor_value_to_orientation(const struct imu_data *data,
				    double *orientation_out)
{
	if (data == NULL || orientation_out == NULL)
		return -EINVAL;

	double x = out_ev(&data->accel[0]);
	double y = out_ev(&data->accel[1]);
	*orientation_out = atan2(y, x) * (180.0 / M_PI);
	return 0;
}
