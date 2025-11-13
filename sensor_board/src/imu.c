/*
 * Copyright (c) 2025, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <lib/imu.h>

static inline float out_ev(struct sensor_value *val)
{
	return (val->val1 + (float)val->val2 / 1000000);
}

static void fetch_and_display(const struct device *dev)
{
	struct sensor_value x, y, z;
	static int trig_cnt;

	trig_cnt++;

	/* lsm6dso accel */
	sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_X, &x);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Y, &y);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Z, &z);

	printf("accel x:%f ms/2 y:%f ms/2 z:%f ms/2\n",
			(double)out_ev(&x), (double)out_ev(&y), (double)out_ev(&z));

	/* lsm6dso gyro */
	sensor_sample_fetch_chan(dev, SENSOR_CHAN_GYRO_XYZ);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_X, &x);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_Y, &y);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_Z, &z);

	printf("gyro x:%f rad/s y:%f rad/s z:%f rad/s\n",
			(double)out_ev(&x), (double)out_ev(&y), (double)out_ev(&z));

	printf("trig_cnt:%d\n\n", trig_cnt);
}

int imu_set_sampling_freq(const struct device *dev, float sampling_rate_hz)
{
	int ret = 0;
	struct sensor_value odr_attr;

	/* set accel/gyro sampling frequency */
	odr_attr.val1 = sampling_rate_hz;
	odr_attr.val2 = 0;

	ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
			SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
	if (ret != 0) {
		printf("Cannot set sampling frequency for accelerometer.\n");
		return ret;
	}

	ret = sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ,
			SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
	if (ret != 0) {
		printf("Cannot set sampling frequency for gyro.\n");
		return ret;
	}

	return 0;
}

#if defined(CONFIG_LSM6DSO_TRIGGER)
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
		printf("Could not set sensor type and channel\n");
		return;
	}
}

#else
void imu_poll(const struct device *dev)
{
	fetch_and_display(dev);
}
#endif

int imu_init(const struct device *dev)
{
    float imu_hz;

	if (!device_is_ready(dev)) {
		printk("%s: device not ready.\n", dev->name);
		return -ENODEV;
	}

    imu_hz = strtof(CONFIG_IMU_HZ, NULL);
    if (imu_set_sampling_freq(dev, imu_hz) != 0) {
		return -EIO;
	}

#ifdef CONFIG_LSM6DSO_TRIGGER
	printf("Testing LSM6DSO sensor in trigger mode.\n\n");
	run_trigger_mode(dev);
#endif
	return 0;
}
