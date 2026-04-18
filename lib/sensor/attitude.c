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
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/attitude.h>

LOG_MODULE_REGISTER(attitude, CONFIG_AURORA_SENSORS_LOG_LEVEL);

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

/* attitude_calibrate_sample – see attitude.h */
int attitude_calibrate_sample(struct attitude *att,
			      const double accel[ATTITUDE_NUM_AXES],
			      const double gyro[ATTITUDE_NUM_AXES])
{
	if (att == NULL || accel == NULL || gyro == NULL)
		return -EINVAL;

	if (att->calibrated)
		return -EALREADY;

	for (int i = 0; i < ATTITUDE_NUM_AXES; i++) {
		att->cal_accel_sum[i] += accel[i];
		att->cal_gyro_sum[i] += gyro[i];
	}
	att->cal_samples++;

	return 0;
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
