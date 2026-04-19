/**
 * @file kalman.c
 * @brief Kalman filter for apogee detection.
 *
 * Implements a 2-state (altitude, vertical velocity) Kalman filter that
 * tracks barometric altitude and detects apogee via a velocity zero-crossing.
 *
 * Copyright (c) 2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/filter.h>

LOG_MODULE_REGISTER(kalman, CONFIG_AURORA_FILTER_LOG_LEVEL);

/* filter_init – see filter.h */
int filter_init(struct filter *filter)
{
	if (filter == NULL)
	return -EINVAL;

	const double q_alt =
	((double)CONFIG_FILTER_Q_ALT_MILLISCALE) / FILTER_SCALE_DIVISOR;

	const double q_vel =
	((double)CONFIG_FILTER_Q_VEL_MILLISCALE) / FILTER_SCALE_DIVISOR;

	const double r_meas =
	((double)CONFIG_FILTER_R_MILLISCALE) / FILTER_SCALE_DIVISOR;

	filter->state[0] = 0.0;
	filter->state[1] = 0.0;

	filter->covariance[0][0] = 10.0;
	filter->covariance[0][1] = 0.0;
	filter->covariance[1][0] = 0.0;
	filter->covariance[1][1] = 10.0;

	filter->noise_p[0][0] = q_alt;
	filter->noise_p[0][1] = 0.0;
	filter->noise_p[1][0] = 0.0;
	filter->noise_p[1][1] = q_vel;

	filter->noise_m = r_meas;

	filter->peak_altitude = 0.0;
	filter->last_accel_vert = 0.0;
	filter->consecutive_apogee = 0;
	filter->apogee_latched = 0;

	return 0;
}

/* filter_predict – see filter.h */
int filter_predict(struct filter *filter, int64_t dt, double a_vert)
{
	if (filter == NULL || dt <= 0)
	return -EINVAL;

	const double dt_s = (double)dt / 1e9;

	/* Clamp dt to prevent filter explosion */
	if (dt_s > 1.0)
	return -EINVAL;

	/* State prediction with a_vert as control input
	 * (F unchanged; B = [0.5*dt^2, dt]^T applied to a_vert) */
	const double altitude = filter->state[0];
	const double velocity = filter->state[1];

	filter->state[0] = altitude + velocity * dt_s + 0.5 * a_vert * dt_s * dt_s;
	filter->state[1] = velocity + a_vert * dt_s;
	filter->last_accel_vert = a_vert;

	/* Scale process noise with dt */
	const double Q00 = filter->noise_p[0][0] * dt_s;
	const double Q11 = filter->noise_p[1][1] * dt_s;

	/* Covariance prediction */
	const double P00 = filter->covariance[0][0];
	const double P01 = filter->covariance[0][1];
	const double P10 = filter->covariance[1][0];
	const double P11 = filter->covariance[1][1];

	filter->covariance[0][0] = P00 + dt_s*(P10 + P01) + dt_s*dt_s*P11 + Q00;
	filter->covariance[0][1] = P01 + dt_s*P11;
	filter->covariance[1][0] = P10 + dt_s*P11;
	filter->covariance[1][1] = P11 + Q11;

	return 0;
}

/* filter_update – see filter.h */
int filter_update(struct filter *filter, double z)
{
	if (filter == NULL)
	return -EINVAL;

	/* Innovation */
	double y = z - filter->state[0];

	/* Innovation covariance */
	double S = filter->covariance[0][0] + filter->noise_m;

	if (fabs(S) < 1e-12)
	return -EDOM;

	/* Kalman gain */
	const double K0 = filter->covariance[0][0] / S;
	const double K1 = filter->covariance[1][0] / S;

	/* State update */
	filter->state[0] += K0 * y;
	filter->state[1] += K1 * y;

	/* Covariance update */
	const double P00 = filter->covariance[0][0];
	const double P01 = filter->covariance[0][1];
	const double P10 = filter->covariance[1][0];
	const double P11 = filter->covariance[1][1];

	filter->covariance[0][0] = P00 - K0 * P00;
	filter->covariance[0][1] = P01 - K0 * P01;
	filter->covariance[1][0] = P10 - K1 * P00;
	filter->covariance[1][1] = P11 - K1 * P01;

	return 0;
}

/* filter_detect_apogee – see filter.h */
int filter_detect_apogee(struct filter *filter)
{
	if (filter == NULL)
	return -EINVAL;

	const double altitude = filter->state[0];
	const double velocity = filter->state[1];

	/* Always track peak, even after latching, so a re-init starts fresh. */
	if (altitude > filter->peak_altitude)
	filter->peak_altitude = altitude;

	if (filter->apogee_latched)
	return 0;

	const double delta_h =
	((double)CONFIG_FILTER_APOGEE_DELTA_H_CM) / 100.0;
	const double accel_max =
	((double)CONFIG_FILTER_APOGEE_ACCEL_MAX_MILLI) / FILTER_SCALE_DIVISOR;

	const int velocity_ok = velocity <= 0.0;
	const int descent_ok = altitude <= filter->peak_altitude - delta_h;
	const int accel_ok = filter->last_accel_vert < accel_max;

	if (velocity_ok && descent_ok && accel_ok) {
	filter->consecutive_apogee++;
	} else {
	filter->consecutive_apogee = 0;
	}

	if (filter->consecutive_apogee >= CONFIG_FILTER_APOGEE_DEBOUNCE_SAMPLES) {
	filter->apogee_latched = 1;
	return 1;
	}

	return 0;
}
