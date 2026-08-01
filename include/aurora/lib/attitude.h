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
 *   2. While the rocket is stationary (e.g. in SM_IDLE), call
 *      @ref attitude_calibrate_sample() each IMU update to accumulate
 *      accelerometer/gyroscope bias and gravity magnitude. Three
 *      independent checks treat a sample as motion/mis-orientation and
 *      discard the accumulator, restarting the window from the next
 *      sample, so a bump, re-aim mid-window, or attempt that never held
 *      the right orientation in the first place can't silently corrupt
 *      the bias estimate: the gyroscope magnitude exceeding
 *      @c CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG (deg/s); the
 *      accelerometer direction tilting more than that same value (read
 *      as degrees) away from the direction recorded at the start of the
 *      window; or the accelerometer direction tilting more than that
 *      value away from the configured mounting axis
 *      (@c CONFIG_IMU_UP_AXIS_*) at all, regardless of the window's own
 *      start.
 *   3. Poll @ref attitude_calibrate_converged() each update once
 *      accumulating; it reports ready once the running gyro-bias mean
 *      has stopped meaningfully changing between checkpoints, or once a
 *      fixed multiple of @c CONFIG_IMU_CALIBRATION_SAMPLES is reached as
 *      a safety ceiling, rather than waiting for a fixed sample count.
 *   4. @ref attitude_calibrate_finish() seals the biases and seeds the
 *      body-frame gravity unit vector from the mounting-axis Kconfig
 *      (@c CONFIG_IMU_UP_AXIS_*).
 *   5. During flight, @ref attitude_update() integrates gyro into the
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

	/** Running gyro-bias mean at the last convergence checkpoint. */
	double cal_checkpoint_gyro_mean[ATTITUDE_NUM_AXES];
	/** cal_samples value at the last convergence checkpoint, 0 = none yet. */
	int cal_checkpoint_samples;
	/** Non-zero once two consecutive checkpoints agreed within threshold. */
	int cal_converged;
	/** Normalized accelerometer direction at the start of the current
	 *  calibration window, used to detect tilt deviation.
	 */
	double cal_accel_ref[ATTITUDE_NUM_AXES];

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
 * @brief Query whether the calibration accumulator is ready to finish.
 *
 * True once the running gyro-bias mean has stopped changing meaningfully
 * between convergence checkpoints, or once a fixed multiple of
 * @c CONFIG_IMU_CALIBRATION_SAMPLES samples have been accumulated
 * (a safety ceiling), whichever comes first. Always false before
 * @c CONFIG_IMU_CALIBRATION_SAMPLES samples have been accumulated.
 *
 * @param att Pointer to tracker state.
 *
 * @retval 1 if ready to call @ref attitude_calibrate_finish().
 * @retval 0 if not yet ready.
 * @retval -EINVAL if @p att is NULL.
 */
int attitude_calibrate_converged(const struct attitude *att);

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
