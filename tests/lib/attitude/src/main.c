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
