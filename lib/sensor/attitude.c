/**
 * @file attitude.c
 * @brief Gyro-integrated body-frame gravity tracker.
 *
 * Tracks gravity direction in IMU body frame by integrating gyro
 * measurements, anchored to an initial direction set from the
 * mounting-axis Kconfig after a stationary calibration window.
 *
 * Copyright (c) 2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/attitude.h>

LOG_MODULE_REGISTER(attitude, CONFIG_AURORA_SENSORS_LOG_LEVEL);

#ifndef M_PI
#define M_PI ((double)3.1415926535)
#endif

/* Internal tuning constants for the convergence/safety-ceiling logic.
 * Not exposed via Kconfig: neither is something an operator needs to
 * tune per campaign, unlike CONFIG_IMU_CALIBRATION_SAMPLES (how long,
 * minimum) or CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG (how
 * twitchy is the pad).
 */
#define CAL_CONVERGENCE_CHECK_INTERVAL 20
#define CAL_MAX_SAMPLES_MULTIPLIER 5

static inline void vec3_zero(double v[3])
{
	v[0] = 0.0;
	v[1] = 0.0;
	v[2] = 0.0;
}

static inline double vec3_norm(const double v[3])
{
	return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* attitude_init – see attitude.h */
int attitude_init(struct attitude *att)
{
	if (att == NULL)
		return -EINVAL;

	memset(att, 0, sizeof(*att));

	/* Seed g_b from the mounting axis.  g_b points in the direction of
	 * gravity in body frame, i.e. opposite to the "up" axis.
	 */
	const int idx = CONFIG_IMU_UP_AXIS_INDEX;
	const int sign = CONFIG_IMU_UP_AXIS_SIGN;

	att->g_b[idx] = (double)(-sign);

	/* Provisional gravity magnitude; overwritten by calibration. */
	att->g_mag = 9.80665;

	return 0;
}

/* Discard the calibration accumulator and restart the window from the
 * next sample. Shared by both the gyro-rate and accelerometer-tilt
 * restart triggers in attitude_calibrate_sample().
 */
static void cal_restart(struct attitude *att, const char *reason)
{
	if (att->cal_samples > 0) {
		LOG_INF("Calibration restarted: %s (%d samples discarded)",
			reason, att->cal_samples);
	}
	att->cal_samples = 0;
	vec3_zero(att->cal_accel_sum);
	vec3_zero(att->cal_gyro_sum);
	vec3_zero(att->cal_accel_ref);
	att->cal_checkpoint_samples = 0;
	vec3_zero(att->cal_checkpoint_gyro_mean);
	att->cal_converged = 0;
}

/* attitude_calibrate_sample – see attitude.h */
int attitude_calibrate_sample(struct attitude *att,
			      const double accel[ATTITUDE_NUM_AXES],
			      const double gyro[ATTITUDE_NUM_AXES])
{
	if (att == NULL || accel == NULL || gyro == NULL)
		return -EINVAL;

	if (att->calibrated)
		return -EALREADY;

	const double deg2rad = M_PI / 180.0;
	const double restart_thresh_rad =
		(double)CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG * deg2rad;

	/* Trigger 1: instantaneous gyro rate. */
	if (vec3_norm(gyro) > restart_thresh_rad) {
		cal_restart(att, "gyro rate");
		return 0;
	}

	/* Trigger 2: accelerometer direction tilted away from the window's
	 * reference direction (recorded below from the window's first
	 * sample). Skipped on a degenerate (near-zero) accel reading.
	 */
	const double accel_norm = vec3_norm(accel);
	double accel_dir[ATTITUDE_NUM_AXES] = {0};
	const bool have_accel_dir = accel_norm > 1e-6;

	if (have_accel_dir) {
		for (int i = 0; i < ATTITUDE_NUM_AXES; i++) {
			accel_dir[i] = accel[i] / accel_norm;
		}
	}

	/* Trigger 3: accelerometer direction tilted away from the
	 * configured mounting axis (g_b, seeded in attitude_init() from
	 * CONFIG_IMU_UP_AXIS_*). Absolute, unlike trigger 2 below (which is
	 * relative to the window's own start): catches a calibration
	 * attempt that never held the correct orientation in the first
	 * place, which trigger 2 alone would miss since it has nothing to
	 * compare against until a second sample arrives.
	 */
	if (have_accel_dir) {
		double dot_up = -(accel_dir[0] * att->g_b[0] +
				   accel_dir[1] * att->g_b[1] +
				   accel_dir[2] * att->g_b[2]);

		if (dot_up > 1.0)
			dot_up = 1.0;
		if (dot_up < -1.0)
			dot_up = -1.0;

		if (acos(dot_up) > restart_thresh_rad) {
			cal_restart(att, "held out of tolerance");
			return 0;
		}
	}

	if (have_accel_dir && att->cal_samples > 0) {
		double dot = accel_dir[0] * att->cal_accel_ref[0] +
			     accel_dir[1] * att->cal_accel_ref[1] +
			     accel_dir[2] * att->cal_accel_ref[2];

		if (dot > 1.0)
			dot = 1.0;
		if (dot < -1.0)
			dot = -1.0;

		if (acos(dot) > restart_thresh_rad) {
			cal_restart(att, "accelerometer tilt");
			return 0;
		}
	}

	if (att->cal_samples == 0 && have_accel_dir) {
		memcpy(att->cal_accel_ref, accel_dir, sizeof(accel_dir));
	}

	for (int i = 0; i < ATTITUDE_NUM_AXES; i++) {
		att->cal_accel_sum[i] += accel[i];
		att->cal_gyro_sum[i] += gyro[i];
	}
	att->cal_samples++;

	/* Checkpoint the running gyro-bias mean every CHECK_INTERVAL samples
	 * once the minimum window has elapsed, and compare against the
	 * previous checkpoint to detect convergence (see
	 * attitude_calibrate_converged()).
	 */
	if (att->cal_samples >= CONFIG_IMU_CALIBRATION_SAMPLES &&
	    (att->cal_samples - att->cal_checkpoint_samples) >=
		    CAL_CONVERGENCE_CHECK_INTERVAL) {
		double mean[ATTITUDE_NUM_AXES];

		for (int i = 0; i < ATTITUDE_NUM_AXES; i++) {
			mean[i] = att->cal_gyro_sum[i] / (double)att->cal_samples;
		}

		if (att->cal_checkpoint_samples > 0) {
			double delta[ATTITUDE_NUM_AXES];

			for (int i = 0; i < ATTITUDE_NUM_AXES; i++) {
				delta[i] = mean[i] - att->cal_checkpoint_gyro_mean[i];
			}

			/* Convergence threshold derives from the same
			 * motion-sensitivity dial as the restart triggers
			 * above, scaled down by 100x (default 5 deg/s ->
			 * 0.05 deg/s).
			 */
			const double conv_thresh_rad_s =
				restart_thresh_rad / 100.0;

			att->cal_converged = (vec3_norm(delta) < conv_thresh_rad_s) ? 1 : 0;
		}

		memcpy(att->cal_checkpoint_gyro_mean, mean, sizeof(mean));
		att->cal_checkpoint_samples = att->cal_samples;
	}

	return 0;
}

/* attitude_calibrate_converged – see attitude.h */
int attitude_calibrate_converged(const struct attitude *att)
{
	if (att == NULL)
		return -EINVAL;

	if (att->cal_samples < CONFIG_IMU_CALIBRATION_SAMPLES)
		return 0;

	if (att->cal_samples >= CONFIG_IMU_CALIBRATION_SAMPLES * CAL_MAX_SAMPLES_MULTIPLIER)
		return 1;

	return att->cal_converged ? 1 : 0;
}

/* attitude_calibrate_finish – see attitude.h */
int attitude_calibrate_finish(struct attitude *att)
{
	if (att == NULL)
		return -EINVAL;

	if (att->cal_samples <= 0)
		return -ENODATA;

	const double n = (double)att->cal_samples;
	double accel_mean[ATTITUDE_NUM_AXES];

	for (int i = 0; i < ATTITUDE_NUM_AXES; i++) {
		accel_mean[i] = att->cal_accel_sum[i] / n;
		att->gyro_bias[i] = att->cal_gyro_sum[i] / n;
	}

	/* Gravity magnitude is the norm of the averaged accelerometer
	 * reading (specific force = -g when stationary).
	 */
	double g_mag = vec3_norm(accel_mean);
	if (g_mag < 1e-6) {
		LOG_WRN("Calibration accel magnitude near zero (%f); "
			"falling back to 9.80665", g_mag);
		g_mag = 9.80665;
	}
	att->g_mag = g_mag;

	/* Seed g_b from the mounting-axis Kconfig.  Accel bias is the
	 * residual after removing the gravity contribution along g_b.
	 * Body-frame specific force when stationary: f_b = -g_mag * g_b.
	 * So accel_bias = accel_mean - (-g_mag * g_b) = accel_mean + g_mag * g_b.
	 */
	vec3_zero(att->g_b);
	const int idx = CONFIG_IMU_UP_AXIS_INDEX;
	const int sign = CONFIG_IMU_UP_AXIS_SIGN;
	att->g_b[idx] = (double)(-sign);

	for (int i = 0; i < ATTITUDE_NUM_AXES; i++) {
		att->accel_bias[i] = accel_mean[i] + g_mag * att->g_b[i];
	}

	att->calibrated = 1;

	LOG_INF("Attitude calibrated: n=%d g_mag=%.3f g_b=[%.2f %.2f %.2f]",
		att->cal_samples, g_mag, att->g_b[0], att->g_b[1], att->g_b[2]);
	LOG_DBG("gyro_bias=[%.4f %.4f %.4f] accel_bias=[%.4f %.4f %.4f]",
		att->gyro_bias[0], att->gyro_bias[1], att->gyro_bias[2],
		att->accel_bias[0], att->accel_bias[1], att->accel_bias[2]);

	return 0;
}

/* attitude_update – see attitude.h */
int attitude_update(struct attitude *att,
		    const double accel[ATTITUDE_NUM_AXES],
		    const double gyro[ATTITUDE_NUM_AXES],
		    double dt_s,
		    double *accel_vert_out)
{
	if (att == NULL || accel == NULL || gyro == NULL ||
	    accel_vert_out == NULL || dt_s <= 0.0)
		return -EINVAL;

	if (!att->calibrated)
		return -ENODATA;

	/* Bias-correct inputs. */
	const double ax = accel[0] - att->accel_bias[0];
	const double ay = accel[1] - att->accel_bias[1];
	const double az = accel[2] - att->accel_bias[2];

	const double wx = (gyro[0] - att->gyro_bias[0]) * dt_s;
	const double wy = (gyro[1] - att->gyro_bias[1]) * dt_s;
	const double wz = (gyro[2] - att->gyro_bias[2]) * dt_s;

	/* Small-angle rotation of gravity vector: dg_b/dt = -omega x g_b.
	 * Increment: g_b_new = g_b - (omega x g_b) * dt.
	 */
	const double gx = att->g_b[0];
	const double gy = att->g_b[1];
	const double gz = att->g_b[2];

	double nx = gx - (wy * gz - wz * gy);
	double ny = gy - (wz * gx - wx * gz);
	double nz = gz - (wx * gy - wy * gx);

	/* Complementary correction toward -a/|a|.  The correction is weighted
	 * by a Gaussian in (|a| - g_mag)/g_mag so the anchor is strong during
	 * quasi-static phases (pad, coast, terminal descent) and smoothly
	 * vanishes during boost and deployment shocks — no hard gate to fall
	 * off.  Gain is dt/tau so behavior is independent of sample rate.
	 */
	const double a_norm = sqrt(ax * ax + ay * ay + az * az);
	if (a_norm > 1e-6) {
		static const double tau_s = 0.5;     /* anchor time constant */
		static const double sigma_r = 0.20;  /* 1-sigma mag band, x g_mag */
		const double r = (a_norm - att->g_mag) / att->g_mag;
		const double w = exp(-0.5 * r * r / (sigma_r * sigma_r));
		const double gain = w * dt_s / tau_s;
		const double inv = 1.0 / a_norm;
		const double gmx = -ax * inv;
		const double gmy = -ay * inv;
		const double gmz = -az * inv;
		nx += gain * (gmx - nx);
		ny += gain * (gmy - ny);
		nz += gain * (gmz - nz);
	}

	/* Renormalize to unit length. */
	const double n = sqrt(nx * nx + ny * ny + nz * nz);
	if (n < 1e-9) {
		/* Numerical collapse – refuse to update. */
		return -EDOM;
	}
	att->g_b[0] = nx / n;
	att->g_b[1] = ny / n;
	att->g_b[2] = nz / n;

	/* Project body specific force onto world up: f_vert = -dot(a_b, g_b).
	 * Subtract gravity magnitude to get gravity-removed vertical accel.
	 */
	const double f_vert = -(ax * att->g_b[0] + ay * att->g_b[1] + az * att->g_b[2]);
	*accel_vert_out = f_vert - att->g_mag;

	return 0;
}

/* attitude_is_calibrated – see attitude.h */
int attitude_is_calibrated(const struct attitude *att)
{
	if (att == NULL)
		return -EINVAL;

	return att->calibrated ? 1 : 0;
}
