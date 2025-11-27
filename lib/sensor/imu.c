/*
 * Copyright (c) 2025, Auxspace e.V.
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

#include <lib/imu.h>

#ifndef M_PI
#define M_PI ((float)3.1415926535)
#endif

LOG_MODULE_REGISTER(imu, CONFIG_AURORA_SENSORS_LOG_LEVEL);

static inline float out_ev(struct sensor_value *val)
{
	return (val->val1 + (float)val->val2 / 1000000);
}

static void fetch_accel(const struct device *dev, struct sensor_value *x,
						struct sensor_value *y, struct sensor_value *z)
{
	sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_X, x);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Y, y);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Z, z);
}

static void fetch_gyro(const struct device *dev, struct sensor_value *x,
						struct sensor_value *y, struct sensor_value *z)
{
	sensor_sample_fetch_chan(dev, SENSOR_CHAN_GYRO_XYZ);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_X, x);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_Y, y);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_Z, z);
}

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

#if defined(CONFIG_LSM6DSO_TRIGGER)
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

static void trigger_handler(const struct device *dev,
							const struct sensor_trigger *trig)
{
	fetch_and_display(dev);
}

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
int imu_poll(const struct device *dev, float *orientation_deg, float *acc_avg)
{
	struct sensor_value ax, ay, az;

	/* Read accelerometer values */
	fetch_accel(dev, &ax, &ay, &az);

	/* Convert to floating point */
	float x = out_ev(&ax);
	float y = out_ev(&ay);
	float z = out_ev(&az);

	/* Compute magnitude of acceleration */
	float acc = sqrtf(x*x + y*y + z*z);

	/* Compute orientation angle in degrees (atan2(y,x)) */
	float angle_deg = atan2f(y, x) * (180.0f / M_PI);

	/* Return results */
	if (orientation_deg)
		*orientation_deg = angle_deg;

	if (acc_avg)
		*acc_avg = acc;

	return 0;
}
#endif

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

#ifdef CONFIG_LSM6DSO_TRIGGER
	LOG_DBG("Testing IMU in trigger mode.\n\n");
	run_trigger_mode(dev);
#endif

	return 0;
}
