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

#include <aurora/lib/imu.h>

#ifndef M_PI
#define M_PI ((double)3.1415926535)
#endif

LOG_MODULE_REGISTER(imu, CONFIG_AURORA_SENSORS_LOG_LEVEL);

/**
 * @brief Convert a Zephyr sensor_value to a double.
 *
 * @param val Pointer to the sensor value.
 * @return Floating-point representation.
 */
static inline double out_ev(struct sensor_value *val)
{
	return (val->val1 + (double)val->val2 / 1000000);
}

/**
 * @brief Fetch XYZ accelerometer channels from the IMU.
 *
 * @param dev Pointer to the IMU device.
 * @param x   Output for X-axis acceleration.
 * @param y   Output for Y-axis acceleration.
 * @param z   Output for Z-axis acceleration.
 */
static void fetch_accel(const struct device *dev, struct sensor_value *x,
						struct sensor_value *y, struct sensor_value *z)
{
	sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_X, x);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Y, y);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Z, z);
}

#if defined(CONFIG_LSM6DSO_TRIGGER)
/**
 * @brief Fetch and log accelerometer and gyroscope readings.
 *
 * @param dev Pointer to the IMU device.
 */
static void fetch_and_display(const struct device *dev)
{
	struct sensor_value x, y, z;
	static int trig_cnt;

	trig_cnt++;

	/* lsm6dso accel */
	fetch_accel(dev, &x, &y, &z);

	LOG_INF("accel x:%f ms/2 y:%f ms/2 z:%f ms/2\n",
			(double)out_ev(&x), (double)out_ev(&y), (double)out_ev(&z));

	/* lsm6dso gyro */
	fetch_gyro(dev, &x, &y, &z);

	LOG_INF("gyro x:%f rad/s y:%f rad/s z:%f rad/s\n",
			(double)out_ev(&x), (double)out_ev(&y), (double)out_ev(&z));

	LOG_INF("trig_cnt:%d\n\n", trig_cnt);
}

/**
 * @brief Data-ready trigger callback for the IMU.
 *
 * @param dev  Pointer to the IMU device.
 * @param trig Pointer to the sensor trigger descriptor.
 */
static void trigger_handler(const struct device *dev,
							const struct sensor_trigger *trig)
{
	fetch_and_display(dev);
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
int imu_poll(const struct device *dev, double *orientation_deg, double *acc_avg)
{
	struct sensor_value ax, ay, az;

	/* Read accelerometer values */
	fetch_accel(dev, &ax, &ay, &az);

	/* Convert to doubleing point */
	double x = out_ev(&ax);
	double y = out_ev(&ay);
	double z = out_ev(&az);

	/* Compute magnitude of acceleration */
	double acc = sqrt(x*x + y*y + z*z);

	/* Compute orientation angle in degrees (atan2(y,x)) */
	double angle_deg = atan2(y, x) * (180.0 / M_PI);

	/* Return results */
	if (orientation_deg)
		*orientation_deg = (double)angle_deg;

	if (acc_avg)
		*acc_avg = (double)acc;

	return 0;
}
#endif

/* imu_init – see imu.h */
int imu_init(const struct device *dev)
{
	if (!device_is_ready(dev)) {
		LOG_ERR("%s: device not ready.\n", dev->name);
		return -ENODEV;
	}

#ifdef CONFIG_LSM6DSO_TRIGGER
	LOG_DBG("Testing IMU in trigger mode.\n\n");
	run_trigger_mode(dev);
#endif

	return 0;
}
