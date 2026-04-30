/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_IMU_H_
#define APP_LIB_IMU_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>


/**
 * @defgroup lib_imu IMU library
 * @ingroup lib
 * @{
 *
 * @brief AURORA IMU library for avionics telemetry.
 */

/** Number of axes for IMU measurements. */
#define IMU_NUM_AXES 3

/** ZBUS channel for IMU data. */
ZBUS_CHAN_DECLARE(imu_data_chan);

/**
 * @brief IMU measurement data structure.
 *
 * carries the measurement data from the IMU, including accelerometer and
 * gyroscope readings for the x, y, and z axes.  This struct is used as a
 * z-bus message payload for IMU data updates
 */
struct imu_data
{
	struct sensor_value accel[IMU_NUM_AXES]; /**< Latest accelerometer readings (x, y, z). */
	struct sensor_value gyro[IMU_NUM_AXES];  /**< Latest gyroscope readings (x, y, z). */
};

#if !defined(CONFIG_IMU_TRIGGER)
/**
 * @brief Poll the IMU for acceleration data and sends it over the z-bus.
 *
 * @param dev             Pointer to the IMU device.
 *
 * @retval 0 on success.
 * @retval -errno Negative errno on failure.
 */
int imu_poll(const struct device *dev);
#endif /* CONFIG_IMU_TRIGGER */

/**
 * @brief Set the IMU accelerometer and gyroscope sampling frequency.
 *
 * @param dev              Pointer to the IMU device.
 * @param sampling_rate_hz Desired sampling rate in Hertz.
 *
 * @retval 0 on success.
 * @retval -errno Negative errno on failure.
 */
int imu_set_sampling_freq(const struct device *dev, int sampling_rate_hz);

/**
 * @brief Initialize the IMU device.
 *
 * Checks device readiness and configures the sampling frequency.
 *
 * @param dev Pointer to the IMU device.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the device is not ready.
 */
int imu_init(const struct device *dev);

/**
 * @brief calculate the average acceleration from IMU sensor values in m/s^2.
 *
 * @param data  Pointer to the IMU sensor data
 * @param acc_out Output for average acceleration in m/s^2. Must be valid pointer to a double.
 * @retval 0 on success.
 * @retval -EINVAL if @p data or @p acc_out is NULL
 */
int imu_sensor_value_to_acceleration(const struct imu_data *data,
				     double *acc_out);

/**
 * @brief Calculate the orientation (yaw, pitch, roll) from IMU sensor values.
 *
 * Uses @c CONFIG_IMU_UP_AXIS_* to remap the body frame so the configured
 * up-axis aligns with world Z, making the result independent of IMU
 * mounting orientation.  The two remaining body axes are taken in cyclic
 * order as the local forward (X) and lateral (Y) axes.
 *
 * Output convention (degrees):
 *   - orientation[0] = yaw   (rotation about up axis)
 *   - orientation[1] = pitch (elevation of forward axis from horizontal)
 *   - orientation[2] = roll  (rotation about forward axis)
 *
 * Only meaningful while gravity dominates (stationary / quasi-static);
 * during powered flight, derive orientation from the attitude tracker.
 *
 * @note Yaw is unobservable from a static accelerometer reading and is
 *       always reported as 0.
 *
 * @param data        Pointer to the IMU sensor data.
 * @param orientation Output: [yaw, pitch, roll] in degrees.  Must be a
 *                    valid pointer to a 3-element double array.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p data or @p orientation is NULL.
 */
int imu_sensor_value_to_orientation(const struct imu_data *data,
				    double *orientation);

/** @} */

#endif /* APP_LIB_IMU_H_ */
