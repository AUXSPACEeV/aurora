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
#if defined(CONFIG_IMU_WATCHDOG)
#include <aurora/lib/sensor_watchdog.h>
#endif

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

#if defined(CONFIG_IMU_WATCHDOG)
static struct sensor_watchdog imu_wd = {
	.name = "imu",
	.timeout_ms = CONFIG_IMU_WATCHDOG_TIMEOUT_MS,
	.period_ms = CONFIG_IMU_WATCHDOG_PERIOD_MS,
	.recover = imu_recover,
};
#endif

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
		return ret;
	}

#if defined(CONFIG_IMU_WATCHDOG)
	sensor_watchdog_mark_publish(&imu_wd);
#endif
	return 0;
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
		LOG_ERR("%s: device not ready", dev->name);
		return -ENODEV;
	}

#if defined(CONFIG_IMU_TRIGGER)
	LOG_DBG("Enabling IMU in trigger mode");
	run_trigger_mode(dev);
#endif

#if defined(CONFIG_IMU_WATCHDOG)
	imu_wd.dev = dev;
	sensor_watchdog_init(&imu_wd);
#endif

	return 0;
}

#if defined(CONFIG_IMU_WATCHDOG)
void imu_watchdog_run(void)
{
	sensor_watchdog_run(&imu_wd);
}

int imu_recover(const struct device *dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}

	struct sensor_value freq = {
		.val1 = CONFIG_IMU_WATCHDOG_RECOVERY_HZ,
		.val2 = 0,
	};
	int rc;

	rc = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
			     SENSOR_ATTR_SAMPLING_FREQUENCY, &freq);
	if (rc != 0 && rc != -ENOTSUP) {
		LOG_ERR("imu_recover: accel ODR set failed (%d)", rc);
		return rc;
	}

	rc = sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ,
			     SENSOR_ATTR_SAMPLING_FREQUENCY, &freq);
	if (rc != 0 && rc != -ENOTSUP) {
		LOG_ERR("imu_recover: gyro ODR set failed (%d)", rc);
		return rc;
	}

#if defined(CONFIG_IMU_TRIGGER)
	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DATA_READY,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};
	rc = sensor_trigger_set(dev, &trig, trigger_handler);
	if (rc != 0) {
		LOG_ERR("imu_recover: trigger re-arm failed (%d)", rc);
		return rc;
	}
#endif

	return 0;
}
#endif /* CONFIG_IMU_WATCHDOG */

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
				    double dt_s,
				    const double gyro_bias[IMU_NUM_AXES],
				    double *orientation)
{
	if (data == NULL || orientation == NULL)
		return -EINVAL;

	const int idx = CONFIG_IMU_UP_AXIS_INDEX;
	const int sign = CONFIG_IMU_UP_AXIS_SIGN;

	double a[3] = {
		out_ev(&data->accel[0]),
		out_ev(&data->accel[1]),
		out_ev(&data->accel[2]),
	};

	/* Re-map body axes so the configured "up" axis becomes Z-up.  The two
	 * remaining axes are taken in cyclic order as the local X (forward)
	 * and Y (lateral) axes.
	 */
	const int x_idx = (idx + 1) % 3;
	const int y_idx = (idx + 2) % 3;

	const double gx = a[x_idx];
	const double gy = a[y_idx];
	const double gz = (double)sign * a[idx];

	const double rad2deg = 180.0 / M_PI;

	orientation[0] = atan2(gy, gz) * rad2deg;
	orientation[1] = atan2(-gx, sqrt(gy * gy + gz * gz)) * rad2deg;

	/* Roll is rotation about the up (long) axis, unobservable from a
	 * static accelerometer reading.  Integrate the gyro component along
	 * the up axis (with mounting sign) into orientation[2].
	 */
	if (dt_s > 0.0) {
		double w_up = out_ev(&data->gyro[idx]);
		if (gyro_bias != NULL) {
			w_up -= gyro_bias[idx];
		}
		w_up *= (double)sign;
		double roll = orientation[2] + w_up * dt_s * rad2deg;
		/* Wrap to [-180, 180]. */
		roll = fmod(roll + 180.0, 360.0);
		if (roll < 0.0) {
			roll += 360.0;
		}
		orientation[2] = roll - 180.0;
	}

	return 0;
}
