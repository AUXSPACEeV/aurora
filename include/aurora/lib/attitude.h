/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_ATTITUDE_H_
#define APP_LIB_ATTITUDE_H_

/**
 * @defgroup lib_attitude Attitude tracker
 * @ingroup lib
 * @{
 *
 * @brief Tracks the gravity direction in IMU body frame and projects
 *        body-frame accelerometer readings onto the world vertical.
 *
 * Intended flow:
 *
 *   1. @ref attitude_init() at boot.
 *   2. While the rocket is stationary (e.g. in SM_ARMED), call
 *      @ref attitude_calibrate_sample() each IMU update to accumulate
 *      accelerometer/gyroscope bias and gravity magnitude.
 *   3. After enough samples, @ref attitude_calibrate_finish() seals the
 *      biases and seeds the body-frame gravity unit vector from the
 *      mounting-axis Kconfig (@c CONFIG_IMU_UP_AXIS_*).
 *   4. During flight, @ref attitude_update() integrates gyro into the
 *      gravity vector and returns the gravity-removed world-frame
 *      vertical acceleration.
 */

/** @brief Number of axes handled by the attitude tracker. */
#define ATTITUDE_NUM_AXES 3

/**
 * @brief Attitude tracker state.
 */
struct attitude {
	/** Gravity unit vector in body frame (points "down" in world frame). */
	double g_b[ATTITUDE_NUM_AXES];
	/** Measured gravity magnitude in m/s^2. */
	double g_mag;
	/** Estimated accelerometer bias in body frame, m/s^2. */
	double accel_bias[ATTITUDE_NUM_AXES];
	/** Estimated gyroscope bias in body frame, rad/s. */
	double gyro_bias[ATTITUDE_NUM_AXES];

	/** Number of samples accumulated into the calibration sums. */
	int cal_samples;
	/** Running sum of accelerometer samples during calibration. */
	double cal_accel_sum[ATTITUDE_NUM_AXES];
	/** Running sum of gyroscope samples during calibration. */
	double cal_gyro_sum[ATTITUDE_NUM_AXES];

	/** Non-zero once attitude_calibrate_finish() has been called. */
	int calibrated;
};

/**
 * @brief Initialize (or reset) the attitude tracker.
 *
 * Clears calibration sums, biases, and sets the body-frame gravity
 * vector to the axis selected via @c CONFIG_IMU_UP_AXIS_*, pointing
 * opposite to "up" (i.e. in the direction gravity pulls the rocket).
 *
 * @param att Pointer to tracker state.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p att is NULL.
 */
int attitude_init(struct attitude *att);

/**
 * @brief Add one IMU sample to the calibration accumulator.
 *
 * Must only be called while the rocket is stationary.
 *
 * @param att   Pointer to tracker state.
 * @param accel Body-frame accelerometer reading in m/s^2.
 * @param gyro  Body-frame gyroscope reading in rad/s.
 *
 * @retval 0 on success.
 * @retval -EINVAL if any pointer is NULL.
 * @retval -EALREADY if calibration has already been finalized.
 */
int attitude_calibrate_sample(struct attitude *att,
			      const double accel[ATTITUDE_NUM_AXES],
			      const double gyro[ATTITUDE_NUM_AXES]);

/**
 * @brief Finalize calibration and seed the body-frame gravity vector.
 *
 * Averages the accumulated samples to compute accelerometer bias, gyro
 * bias, and gravity magnitude, then seeds @c g_b from the Kconfig
 * mounting axis. After this call @ref attitude_is_calibrated returns
 * non-zero.
 *
 * @param att Pointer to tracker state.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p att is NULL.
 * @retval -ENODATA if no samples have been accumulated.
 */
int attitude_calibrate_finish(struct attitude *att);

/**
 * @brief Propagate the gravity vector with a new IMU sample and project
 *        accelerometer into world vertical.
 *
 * Subtracts biases, rotates the body-frame gravity vector by the
 * gyro-integrated body rotation (small-angle Rodrigues), renormalizes,
 * and returns the gravity-removed world-frame vertical acceleration
 * (positive = up).
 *
 * @param att            Pointer to tracker state.
 * @param accel          Body-frame accelerometer reading in m/s^2.
 * @param gyro           Body-frame gyroscope reading in rad/s.
 * @param dt_s           Elapsed time in seconds since the previous update.
 * @param accel_vert_out Output: world-frame vertical accel in m/s^2,
 *                       gravity-removed (positive = up).
 *
 * @retval 0 on success.
 * @retval -EINVAL if any pointer is NULL or @p dt_s <= 0.
 * @retval -ENODATA if calibration has not been finalized.
 */
int attitude_update(struct attitude *att,
		    const double accel[ATTITUDE_NUM_AXES],
		    const double gyro[ATTITUDE_NUM_AXES],
		    double dt_s,
		    double *accel_vert_out);

/**
 * @brief Query whether calibration has been finalized.
 *
 * @param att Pointer to tracker state.
 *
 * @retval 1 if calibrated, 0 if not, -EINVAL if @p att is NULL.
 */
int attitude_is_calibrated(const struct attitude *att);

/** @} */

#endif /* APP_LIB_ATTITUDE_H_ */
