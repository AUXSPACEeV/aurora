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
#if defined(CONFIG_IMU_MAGNETOMETER)
	struct sensor_value magn[IMU_NUM_AXES];  /**< Latest magnetometer readings (x, y, z), Gauss. */
#endif /* CONFIG_IMU_MAGNETOMETER */
};

/**
 * @brief Take one IMU sample, publish it on the z-bus and hand it back.
 *
 * In trigger mode the sample was already read off the sensor by the driver's
 * data-ready handler; this converts and publishes it. Returns -EAGAIN when no
 * new sample has arrived since the last call, which is not an error: the
 * caller is polling faster than the sensor produces.
 *
 * @param dev Pointer to the IMU device. Ignored by the simulated sources.
 * @param out Optional; receives a copy of the sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN in trigger mode when no new sample is waiting.
 * @retval -errno Negative errno on failure.
 */
int imu_poll(const struct device *dev, struct imu_data *out);

/**
 * @brief Initialize the IMU.
 *
 * Checks device readiness and, with CONFIG_IMU_TRIGGER, installs the
 * library's own data-ready handler.
 *
 * A device that is present but cannot deliver that trigger reports
 * -ENOTSUP: the device is usable, and the caller is expected to fall back
 * to polling it with imu_poll() rather than treat the IMU as absent.
 *
 * @param dev Pointer to the IMU device. Ignored by the simulated sources.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p dev is NULL.
 * @retval -ENODEV if the device is not ready.
 * @retval -ENOTSUP if the device is ready but has no data-ready trigger.
 * @retval -ENODATA if a simulated source has no sample data.
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

#if defined(CONFIG_IMU_MAGNETOMETER)
/**
 * @brief Compute a tilt-compensated magnetic heading from a 9-DoF sample.
 *
 * Uses the accelerometer to recover the unit's tilt and de-rotates the
 * magnetometer into the horizontal plane, so the heading stays valid when
 * the unit is not held level.  The body frame is remapped with the same
 * @c CONFIG_IMU_UP_AXIS_* convention as @ref imu_sensor_value_to_orientation:
 * the configured up-axis becomes world Z and the two remaining axes are the
 * local forward (X) and lateral (Y).  The heading returned is that of the
 * forward axis.
 *
 * Output is the magnetic heading in degrees, 0 = magnetic north, increasing
 * clockwise (0..360).  Magnetic declination is @b not applied — add the local
 * declination at the call site for true north.  The result is only as good as
 * the magnetometer calibration: hard/soft-iron offsets must be removed
 * upstream, and the sign/zero reference may need a per-unit offset depending
 * on how the sensor is mounted.
 *
 * @param data        Pointer to the IMU sample (accelerometer + magnetometer).
 * @param heading_out Output: magnetic heading of the forward axis in degrees,
 *                    normalized to [0, 360).  Must be a valid pointer.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p data or @p heading_out is NULL.
 */
int imu_sensor_value_to_heading(const struct imu_data *data, double *heading_out);
#endif /* CONFIG_IMU_MAGNETOMETER */

/** @} */

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_imu_data(const struct imu_data *imu);
#else
static inline void log_imu_data(const struct imu_data *imu) {}
#endif
#endif /* APP_LIB_IMU_H_ */
