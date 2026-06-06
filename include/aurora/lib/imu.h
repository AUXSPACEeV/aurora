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

#if defined(CONFIG_IMU_WATCHDOG)
/**
 * @brief Run the IMU stall watchdog. Never returns.
 *
 * Intended as the post-init body of the IMU thread when the IMU is driven by
 * a hardware data-ready trigger (no polling loop).
 */
void imu_watchdog_run(void);

/**
 * @brief Attempt to recover a stalled IMU.
 *
 * Re-applies @c CONFIG_IMU_WATCHDOG_RECOVERY_HZ to the accelerometer and
 * gyroscope and re-arms the data-ready trigger. Intended to recover from a
 * soft-reset caused by a power-rail transient.
 *
 * @param dev Pointer to the IMU device.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p dev is NULL.
 * @retval -errno from the underlying sensor API on failure.
 */
int imu_recover(const struct device *dev);
#endif /* CONFIG_IMU_WATCHDOG */

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
 *   - orientation[0] = yaw   (tilt of the forward axis from horizontal)
 *   - orientation[1] = pitch (tilt of the lateral axis from horizontal)
 *   - orientation[2] = roll  (rotation about the up axis — the rocket's
 *                             long axis / flight path)
 *
 * Yaw and pitch are derived from the accelerometer (gravity-dominated,
 * meaningful only during quasi-static phases).  Roll is unobservable
 * from a static accelerometer reading because it is the rotation about
 * the gravity vector itself; it is integrated from the gyroscope.
 *
 * The caller owns the roll state: @p orientation[2] is read on input as
 * the previous roll angle, advanced by @c gyro_up * dt_s, and written
 * back wrapped to [-180, 180] degrees.  Pass @p dt_s <= 0 to leave the
 * roll value untouched (e.g. on the very first sample, before a dt is
 * known).
 *
 * @param data        Pointer to the IMU sensor data.
 * @param dt_s        Elapsed time in seconds since the previous call,
 *                    used to integrate roll.  Pass 0 to skip
 *                    integration (yaw and pitch are still updated).
 * @param gyro_bias   Optional bias to subtract from the gyro reading
 *                    before integration, in rad/s.  Pass NULL to skip
 *                    bias correction.
 * @param orientation In/out: [yaw, pitch, roll] in degrees.  Yaw and
 *                    pitch are overwritten; roll is read and updated.
 *                    Must be a valid pointer to a 3-element double array.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p data or @p orientation is NULL.
 */
int imu_sensor_value_to_orientation(const struct imu_data *data,
				    double dt_s,
				    const double gyro_bias[IMU_NUM_AXES],
				    double *orientation);

/** @} */

#endif /* APP_LIB_IMU_H_ */
