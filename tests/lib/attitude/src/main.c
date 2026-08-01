/**
 * @file main.c
 * @brief Unit tests for the attitude (body-frame gravity) tracker.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <errno.h>
#include <zephyr/ztest.h>
#include <aurora/lib/attitude.h>

#ifndef M_PI
#define M_PI ((double)3.14159265358979323846)
#endif

#define FLOAT_TOL 0.001

#define zassert_near(a, b, tol, msg) \
	zassert_true(fabs((a) - (b)) < (tol), msg)

/* Kconfig defaults: +Z up -> INDEX=2, SIGN=+1 -> g_b seeded to [0,0,-1]. */

static struct attitude att;

static void setup(void *f)
{
	ARG_UNUSED(f);
	attitude_init(&att);
}

ZTEST_SUITE(attitude_tests, NULL, NULL, setup, NULL, NULL);

ZTEST(attitude_tests, test_init_seeds_gravity_down_axis)
{
	/* With default CONFIG_IMU_UP_AXIS_POS_Z, g_b points to -Z. */
	zassert_near(att.g_b[0], 0.0, FLOAT_TOL, "g_b[0] should be 0");
	zassert_near(att.g_b[1], 0.0, FLOAT_TOL, "g_b[1] should be 0");
	zassert_near(att.g_b[2], -1.0, FLOAT_TOL, "g_b[2] should be -1 for +Z up");
	zassert_equal(att.calibrated, 0, "Freshly initialized tracker is not calibrated");
}

ZTEST(attitude_tests, test_update_before_calibration_errors)
{
	double accel[3] = {0.0, 0.0, 9.81};
	double gyro[3]  = {0.0, 0.0, 0.0};
	double a_v;

	int ret = attitude_update(&att, accel, gyro, 0.01, &a_v);
	zassert_equal(ret, -ENODATA, "update without calibration must fail");
}

ZTEST(attitude_tests, test_calibrate_stationary_produces_zero_bias)
{
	/* Perfect +Z-up stationary sample: accel = [0,0,+g]. */
	double accel[3] = {0.0, 0.0, 9.81};
	double gyro[3]  = {0.0, 0.0, 0.0};

	for (int i = 0; i < 100; i++) {
		zassert_equal(attitude_calibrate_sample(&att, accel, gyro), 0,
			      "calibrate_sample should succeed");
	}
	zassert_equal(attitude_calibrate_finish(&att), 0,
		      "calibrate_finish should succeed");

	zassert_equal(att.calibrated, 1, "tracker should be calibrated");
	zassert_near(att.g_mag, 9.81, FLOAT_TOL, "gravity magnitude = 9.81");
	zassert_near(att.accel_bias[0], 0.0, FLOAT_TOL, "no X accel bias");
	zassert_near(att.accel_bias[1], 0.0, FLOAT_TOL, "no Y accel bias");
	zassert_near(att.accel_bias[2], 0.0, FLOAT_TOL, "no Z accel bias after gravity subtraction");
	zassert_near(att.gyro_bias[0], 0.0, FLOAT_TOL, "no gyro bias");
	zassert_near(att.gyro_bias[2], 0.0, FLOAT_TOL, "no gyro bias");
}

ZTEST(attitude_tests, test_calibrate_captures_gyro_bias)
{
	/* Stationary +Z-up but with a constant gyro offset of +0.05 rad/s on X. */
	double accel[3] = {0.0, 0.0, 9.81};
	double gyro[3]  = {0.05, 0.0, 0.0};

	for (int i = 0; i < 200; i++) {
		attitude_calibrate_sample(&att, accel, gyro);
	}
	zassert_equal(attitude_calibrate_finish(&att), 0, "finish ok");

	zassert_near(att.gyro_bias[0], 0.05, FLOAT_TOL, "captured gyro X bias");
}

ZTEST(attitude_tests, test_update_stationary_returns_zero_accel_vert)
{
	double accel[3] = {0.0, 0.0, 9.81};
	double gyro[3]  = {0.0, 0.0, 0.0};
	double a_v;

	for (int i = 0; i < 100; i++) {
		attitude_calibrate_sample(&att, accel, gyro);
	}
	attitude_calibrate_finish(&att);

	zassert_equal(attitude_update(&att, accel, gyro, 0.01, &a_v), 0,
		      "update ok");
	zassert_near(a_v, 0.0, FLOAT_TOL, "stationary: a_vert should be ~0");
}

ZTEST(attitude_tests, test_update_positive_boost_along_up_axis)
{
	/* Calibrate at rest, then apply +20 m/s^2 along +Z (up). */
	double rest[3]  = {0.0, 0.0, 9.81};
	double gyro[3]  = {0.0, 0.0, 0.0};
	double boost[3] = {0.0, 0.0, 9.81 + 20.0};
	double a_v;

	for (int i = 0; i < 100; i++) {
		attitude_calibrate_sample(&att, rest, gyro);
	}
	attitude_calibrate_finish(&att);

	zassert_equal(attitude_update(&att, boost, gyro, 0.01, &a_v), 0, "update ok");
	zassert_near(a_v, 20.0, FLOAT_TOL, "boost: +20 m/s^2 vertical accel");
}

ZTEST(attitude_tests, test_update_freefall_returns_negative_g)
{
	double rest[3]     = {0.0, 0.0, 9.81};
	double gyro[3]     = {0.0, 0.0, 0.0};
	double freefall[3] = {0.0, 0.0, 0.0};
	double a_v;

	for (int i = 0; i < 100; i++) {
		attitude_calibrate_sample(&att, rest, gyro);
	}
	attitude_calibrate_finish(&att);

	zassert_equal(attitude_update(&att, freefall, gyro, 0.01, &a_v), 0, "update ok");
	zassert_near(a_v, -9.81, FLOAT_TOL, "freefall: a_vert should be -g");
}

ZTEST(attitude_tests, test_update_rotation_redirects_gravity_vector)
{
	/* Start +Z up, rotate 90 deg about Y at pi/2 rad/s for 1 s.
	 * After rotation: rocket's original +Z is now pointing along world +X,
	 * so in body frame the new "up" axis is body +X. Gravity in body frame
	 * should now lie along -X (i.e. g_b ~ [-1, 0, 0]).
	 *
	 * Apply 100 small steps of dt=0.01s, omega_y = pi/2 rad/s.
	 */
	double rest[3] = {0.0, 0.0, 9.81};
	double gyro0[3] = {0.0, 0.0, 0.0};

	for (int i = 0; i < 100; i++) {
		attitude_calibrate_sample(&att, rest, gyro0);
	}
	attitude_calibrate_finish(&att);

	double omega = M_PI / 2.0;
	double dt = 0.01;
	double gyro[3] = {0.0, omega, 0.0};

	/* Under a pure rotation, the accelerometer reading in body frame must
	 * rotate with the body: a_b = -g_mag * g_b_expected. Feed a consistent
	 * reading so the complementary anchor agrees with the gyro integration
	 * instead of fighting it. Sign convention matches the integrator:
	 * starting g_b = [0,0,-1], omega_y > 0 sweeps g_b toward [+1,0,0], so
	 * g_b(t) = [sin(theta), 0, -cos(theta)] and a_b = [-g sin, 0, g cos].
	 */
	double a_v;
	for (int i = 0; i < 100; i++) {
		double theta = omega * dt * (double)(i + 1);
		double accel[3] = {
			-9.81 * sin(theta),
			0.0,
			 9.81 * cos(theta),
		};
		attitude_update(&att, accel, gyro, dt, &a_v);
	}

	/* After 90 deg rotation about +Y, body +Z (was up) is now body -X
	 * in world terms; equivalently, gravity in body frame moves from -Z
	 * toward +X. Sign convention: omega_y positive rotates +Z toward -X,
	 * so gravity (-Z) rotates toward +X. Expect g_b ~ [+1, 0, 0].
	 */
	double norm = sqrt(att.g_b[0] * att.g_b[0] +
			   att.g_b[1] * att.g_b[1] +
			   att.g_b[2] * att.g_b[2]);
	zassert_near(norm, 1.0, FLOAT_TOL, "g_b stays unit length");
	zassert_true(fabs(att.g_b[0]) > 0.9,
		     "gravity vector should have rotated into the X axis");
	zassert_true(fabs(att.g_b[2]) < 0.2,
		     "gravity vector should have left the Z axis");
}

ZTEST(attitude_tests, test_null_pointers_rejected)
{
	double v[3] = {0.0, 0.0, 0.0};
	double a_v;

	zassert_equal(attitude_init(NULL), -EINVAL, "init NULL rejected");
	zassert_equal(attitude_calibrate_sample(NULL, v, v), -EINVAL, "sample NULL rejected");
	zassert_equal(attitude_calibrate_finish(NULL), -EINVAL, "finish NULL rejected");
	zassert_equal(attitude_update(NULL, v, v, 0.01, &a_v), -EINVAL, "update NULL rejected");
	zassert_equal(attitude_is_calibrated(NULL), -EINVAL, "is_calibrated NULL rejected");
}

ZTEST(attitude_tests, test_finish_without_samples_returns_enodata)
{
	zassert_equal(attitude_calibrate_finish(&att), -ENODATA,
		      "finish with zero samples must fail");
}

ZTEST(attitude_tests, test_converged_false_before_min_samples)
{
	double accel[3] = {0.0, 0.0, 9.81};
	double gyro[3]  = {0.0, 0.0, 0.0};

	zassert_equal(attitude_calibrate_converged(&att), 0,
		      "no samples yet: not converged");

	for (int i = 0; i < CONFIG_IMU_CALIBRATION_SAMPLES - 1; i++) {
		attitude_calibrate_sample(&att, accel, gyro);
	}
	zassert_equal(attitude_calibrate_converged(&att), 0,
		      "below the minimum sample count: not converged");
}

ZTEST(attitude_tests, test_converged_true_once_stationary_mean_settles)
{
	/* Perfectly stationary input: the running mean never moves between
	 * checkpoints, so calibration should converge right after the
	 * minimum sample count plus one checkpoint interval.
	 */
	double accel[3] = {0.0, 0.0, 9.81};
	double gyro[3]  = {0.0, 0.0, 0.0};
	/* +20 mirrors CAL_CONVERGENCE_CHECK_INTERVAL, an internal constant
	 * in attitude.c (not exposed via Kconfig).
	 */
	int n = CONFIG_IMU_CALIBRATION_SAMPLES + 20;

	for (int i = 0; i < n; i++) {
		attitude_calibrate_sample(&att, accel, gyro);
	}

	zassert_equal(attitude_calibrate_converged(&att), 1,
		      "stationary input should converge quickly");
	zassert_equal(attitude_calibrate_finish(&att), 0, "finish ok");
}

ZTEST(attitude_tests, test_converged_true_at_max_samples_ceiling_even_if_noisy)
{
	/* Even if the running mean never settles (cal_converged left
	 * false), hitting the sample-count ceiling must still report ready,
	 * so calibration always terminates in bounded time. Set the
	 * post-loop state directly rather than hunting for a gyro signal
	 * that both stays under the restart threshold and never satisfies
	 * the mean-agreement check for 1000 samples straight (the two
	 * constraints leave essentially no numerically robust margin apart
	 * from the internal logic already covered by the "settles" test).
	 * The *5 mirrors CAL_MAX_SAMPLES_MULTIPLIER, an internal constant
	 * in attitude.c (not exposed via Kconfig).
	 */
	att.cal_samples = CONFIG_IMU_CALIBRATION_SAMPLES * 5;
	att.cal_converged = 0;

	zassert_equal(attitude_calibrate_converged(&att), 1,
		      "max-sample ceiling must force convergence");
}

ZTEST(attitude_tests, test_motion_restarts_calibration_window)
{
	/* A gyro reading above the restart threshold discards the
	 * accumulator instead of polluting the bias estimate.
	 */
	double accel[3] = {0.0, 0.0, 9.81};
	double still[3] = {0.0, 0.0, 0.0};
	double deg2rad = M_PI / 180.0;
	double bump[3] = {
		((double)CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG + 1.0) * deg2rad,
		0.0, 0.0,
	};

	for (int i = 0; i < 50; i++) {
		attitude_calibrate_sample(&att, accel, still);
	}
	zassert_equal(att.cal_samples, 50, "samples accumulated before the bump");

	attitude_calibrate_sample(&att, accel, bump);
	zassert_equal(att.cal_samples, 0,
		      "motion above threshold must discard the accumulator");

	/* Calibration still converges cleanly to a zero bias once the
	 * rocket is stationary again for the rest of the window.
	 */
	for (int i = 0; i < CONFIG_IMU_CALIBRATION_SAMPLES + 20; i++) {
		attitude_calibrate_sample(&att, accel, still);
	}
	zassert_equal(attitude_calibrate_converged(&att), 1,
		      "should converge after the restart once stable again");
	zassert_equal(attitude_calibrate_finish(&att), 0, "finish ok");
	zassert_near(att.gyro_bias[0], 0.0, FLOAT_TOL,
		     "bump samples must not leak into the final bias");
}

ZTEST(attitude_tests, test_sub_threshold_motion_does_not_restart)
{
	double accel[3] = {0.0, 0.0, 9.81};
	double deg2rad = M_PI / 180.0;
	double wobble[3] = {
		((double)CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG - 1.0) * deg2rad,
		0.0, 0.0,
	};

	for (int i = 0; i < 50; i++) {
		attitude_calibrate_sample(&att, accel, wobble);
	}

	zassert_equal(att.cal_samples, 50,
		      "sub-threshold gyro reading must not restart the window");
}

ZTEST(attitude_tests, test_tilt_restarts_calibration_window)
{
	/* An accelerometer reading whose direction has rotated more than
	 * the restart threshold away from the window's starting direction
	 * discards the accumulator too, even with zero gyro rate (a static
	 * re-aim/bump rather than an in-motion one).
	 */
	double still_gyro[3] = {0.0, 0.0, 0.0};
	double rest[3] = {0.0, 0.0, 9.81};
	double deg2rad = M_PI / 180.0;
	double tilt_deg = (double)CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG + 1.0;
	double tilted[3] = {
		9.81 * sin(tilt_deg * deg2rad),
		0.0,
		9.81 * cos(tilt_deg * deg2rad),
	};

	for (int i = 0; i < 50; i++) {
		attitude_calibrate_sample(&att, rest, still_gyro);
	}
	zassert_equal(att.cal_samples, 50, "samples accumulated before the tilt");

	attitude_calibrate_sample(&att, tilted, still_gyro);
	zassert_equal(att.cal_samples, 0,
		      "tilt above threshold must discard the accumulator");

	/* Held at the tilted orientation, it must not accumulate at all: it
	 * is also out of tolerance against the mounting axis, not just the
	 * window's own start (see test_held_out_of_mounting_axis_tolerance_
	 * never_accumulates for that check in isolation). Calibration only
	 * converges once returned to the mounting-axis orientation.
	 */
	attitude_calibrate_sample(&att, tilted, still_gyro);
	zassert_equal(att.cal_samples, 0,
		      "held out of mounting-axis tolerance must still not accumulate");

	for (int i = 0; i < CONFIG_IMU_CALIBRATION_SAMPLES + 20; i++) {
		attitude_calibrate_sample(&att, rest, still_gyro);
	}
	zassert_equal(attitude_calibrate_converged(&att), 1,
		      "should converge after the tilt restart once back within tolerance");
	zassert_equal(attitude_calibrate_finish(&att), 0, "finish ok");
}

ZTEST(attitude_tests, test_sub_threshold_tilt_does_not_restart)
{
	double still_gyro[3] = {0.0, 0.0, 0.0};
	double rest[3] = {0.0, 0.0, 9.81};
	double deg2rad = M_PI / 180.0;
	double tilt_deg = (double)CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG - 1.0;
	double tilted[3] = {
		9.81 * sin(tilt_deg * deg2rad),
		0.0,
		9.81 * cos(tilt_deg * deg2rad),
	};

	attitude_calibrate_sample(&att, rest, still_gyro);
	for (int i = 0; i < 50; i++) {
		attitude_calibrate_sample(&att, tilted, still_gyro);
	}

	zassert_equal(att.cal_samples, 51,
		      "sub-threshold tilt must not restart the window");
}

ZTEST(attitude_tests, test_converged_null_pointer_rejected)
{
	zassert_equal(attitude_calibrate_converged(NULL), -EINVAL,
		      "converged NULL rejected");
}

ZTEST(attitude_tests, test_held_out_of_mounting_axis_tolerance_never_accumulates)
{
	/* Unlike test_tilt_restarts_calibration_window (relative to the
	 * window's own start, so it only fires from the second sample
	 * onward), a sample tilted away from the configured mounting axis
	 * (g_b) from the very first sample must never be allowed to
	 * accumulate at all -- there is no "window start" to compare
	 * against yet, so only the absolute mounting-axis check catches
	 * this.
	 */
	double still_gyro[3] = {0.0, 0.0, 0.0};
	double deg2rad = M_PI / 180.0;
	double tilt_deg = (double)CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG + 1.0;
	double tilted[3] = {
		9.81 * sin(tilt_deg * deg2rad),
		0.0,
		9.81 * cos(tilt_deg * deg2rad),
	};

	for (int i = 0; i < 50; i++) {
		attitude_calibrate_sample(&att, tilted, still_gyro);
		zassert_equal(att.cal_samples, 0,
			      "held out of mounting-axis tolerance must never accumulate");
	}
}

ZTEST(attitude_tests, test_recovers_and_converges_once_within_mounting_axis_tolerance)
{
	double still_gyro[3] = {0.0, 0.0, 0.0};
	double rest[3] = {0.0, 0.0, 9.81};
	double deg2rad = M_PI / 180.0;
	double tilt_deg = (double)CONFIG_IMU_CALIBRATION_RESTART_DEVIATION_DEG + 1.0;
	double tilted[3] = {
		9.81 * sin(tilt_deg * deg2rad),
		0.0,
		9.81 * cos(tilt_deg * deg2rad),
	};

	for (int i = 0; i < 50; i++) {
		attitude_calibrate_sample(&att, tilted, still_gyro);
	}
	zassert_equal(att.cal_samples, 0, "should still be held at zero while out of tolerance");

	for (int i = 0; i < CONFIG_IMU_CALIBRATION_SAMPLES + 20; i++) {
		attitude_calibrate_sample(&att, rest, still_gyro);
	}
	zassert_equal(attitude_calibrate_converged(&att), 1,
		      "should converge once held within the mounting-axis tolerance");
	zassert_equal(attitude_calibrate_finish(&att), 0, "finish ok");
}
