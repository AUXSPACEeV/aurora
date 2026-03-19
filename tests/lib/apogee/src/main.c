/**
 * @file main.c
 * @brief Unit tests for the Kalman filter apogee detection.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <zephyr/ztest.h>
#include <aurora/lib/filter.h>

/** @brief Absolute tolerance for floating-point comparisons. */
#define FLOAT_TOL 0.001f

/** @brief Helper: assert two floats are approximately equal. */
#define zassert_near(a, b, tol, msg) \
	zassert_true(fabsf((a) - (b)) < (tol), msg)

/**
 * @brief Expected noise values from Kconfig defaults.
 *
 * CONFIG_FILTER_Q_ALT_MILLISCALE = 100  -> 0.1
 * CONFIG_FILTER_Q_VEL_MILLISCALE = 500  -> 0.5
 * CONFIG_FILTER_R_MILLISCALE     = 4000 -> 4.0
 */
#define EXPECTED_Q_ALT (100.0f / 1000.0f)
#define EXPECTED_Q_VEL (500.0f / 1000.0f)
#define EXPECTED_R     (4000.0f / 1000.0f)

/** @brief Nanoseconds per millisecond for dt conversion. */
#define NS_PER_MS 1000000LL

static struct filter filt;

static void kalman_filter_before(void *fixture)
{
	filter_init(&filt);
}

ZTEST_SUITE(kalman_filter_tests, NULL, NULL, kalman_filter_before, NULL, NULL);

/* ----------------------------------------------------------------
 * filter_init tests
 * ---------------------------------------------------------------- */

/**
 * @brief Test filter_init with NULL pointer.
 */
ZTEST(kalman_filter_tests, test_init_null)
{
	int ret = filter_init(NULL);
	zassert_equal(ret, -EINVAL, "filter_init(NULL) should return -EINVAL");
}

/**
 * @brief Test filter_init sets correct initial values.
 */
ZTEST(kalman_filter_tests, test_init_values)
{
	struct filter f;
	int ret = filter_init(&f);

	zassert_equal(ret, 0, "filter_init should return 0");

	/* State vector zeroed */
	zassert_near(f.state[0], 0.0f, FLOAT_TOL, "Initial altitude should be 0");
	zassert_near(f.state[1], 0.0f, FLOAT_TOL, "Initial velocity should be 0");

	/* Covariance diagonal = 10, off-diagonal = 0 */
	zassert_near(f.covariance[0][0], 10.0f, FLOAT_TOL, "P[0][0] should be 10");
	zassert_near(f.covariance[0][1], 0.0f, FLOAT_TOL, "P[0][1] should be 0");
	zassert_near(f.covariance[1][0], 0.0f, FLOAT_TOL, "P[1][0] should be 0");
	zassert_near(f.covariance[1][1], 10.0f, FLOAT_TOL, "P[1][1] should be 10");

	/* Process noise from Kconfig defaults */
	zassert_near(f.noise_p[0][0], EXPECTED_Q_ALT, FLOAT_TOL,
		     "Q_alt should match Kconfig");
	zassert_near(f.noise_p[0][1], 0.0f, FLOAT_TOL, "Q[0][1] should be 0");
	zassert_near(f.noise_p[1][0], 0.0f, FLOAT_TOL, "Q[1][0] should be 0");
	zassert_near(f.noise_p[1][1], EXPECTED_Q_VEL, FLOAT_TOL,
		     "Q_vel should match Kconfig");

	/* Measurement noise from Kconfig */
	zassert_near(f.noise_m, EXPECTED_R, FLOAT_TOL,
		     "R should match Kconfig");

	/* Previous velocity for apogee detection */
	zassert_near(f.prev_velocity, 0.0f, FLOAT_TOL,
		     "prev_velocity should be initialized to 0");
}

/* ----------------------------------------------------------------
 * filter_predict tests
 * ---------------------------------------------------------------- */

/**
 * @brief Test filter_predict with NULL pointer.
 */
ZTEST(kalman_filter_tests, test_predict_null)
{
	int ret = filter_predict(NULL, 10 * NS_PER_MS);
	zassert_equal(ret, -EINVAL, "filter_predict(NULL) should return -EINVAL");
}

/**
 * @brief Test filter_predict with zero dt.
 */
ZTEST(kalman_filter_tests, test_predict_zero_dt)
{
	int ret = filter_predict(&filt, 0);
	zassert_equal(ret, -EINVAL, "filter_predict(dt=0) should return -EINVAL");
}

/**
 * @brief Test filter_predict with negative dt.
 */
ZTEST(kalman_filter_tests, test_predict_negative_dt)
{
	int ret = filter_predict(&filt, -100);
	zassert_equal(ret, -EINVAL, "filter_predict(dt<0) should return -EINVAL");
}

/**
 * @brief Test filter_predict propagates state correctly.
 *
 * With altitude=100 and velocity=50, after dt=100ms (0.1s) the predicted
 * altitude should be altitude + velocity * dt_s = 100 + 50 * 0.1 = 105.
 */
ZTEST(kalman_filter_tests, test_predict_state_propagation)
{
	filt.state[0] = 100.0f;  /* altitude */
	filt.state[1] = 50.0f;   /* velocity */

	/* dt = 100ms = 100,000,000 ns -> dt_s = 0.1 */
	int ret = filter_predict(&filt, 100 * NS_PER_MS);
	zassert_equal(ret, 0, "filter_predict should return 0");

	zassert_near(filt.state[0], 105.0f, FLOAT_TOL,
		     "Altitude should be predicted forward by velocity * dt_s");
	zassert_near(filt.state[1], 50.0f, FLOAT_TOL,
		     "Velocity should remain unchanged in constant-velocity model");
}

/**
 * @brief Test filter_predict with small dt.
 *
 * dt = 500,000 ns = 0.5 ms = 0.0005 s.
 * predicted altitude = 100 + 50 * 0.0005 = 100.025.
 */
ZTEST(kalman_filter_tests, test_predict_small_dt)
{
	filt.state[0] = 100.0f;
	filt.state[1] = 50.0f;

	int ret = filter_predict(&filt, 500000LL);  /* 0.5 ms */
	zassert_equal(ret, 0, "filter_predict should return 0");

	zassert_near(filt.state[0], 100.025f, FLOAT_TOL,
		     "Altitude should advance by velocity * 0.0005s");
}

/**
 * @brief Test filter_predict rejects dt exceeding the 1-second clamp.
 */
ZTEST(kalman_filter_tests, test_predict_excessive_dt)
{
	int ret = filter_predict(&filt, 2000000000LL);  /* 2 seconds */
	zassert_equal(ret, -EINVAL,
		      "filter_predict should reject dt > 1s");
}

/**
 * @brief Test filter_predict covariance grows after prediction.
 */
ZTEST(kalman_filter_tests, test_predict_covariance_growth)
{
	float P00_before = filt.covariance[0][0];
	float P11_before = filt.covariance[1][1];

	int ret = filter_predict(&filt, 1 * NS_PER_MS);
	zassert_equal(ret, 0, "filter_predict should return 0");

	/* Covariance should grow due to process noise */
	zassert_true(filt.covariance[0][0] >= P00_before,
		     "P[0][0] should grow after predict");
	zassert_true(filt.covariance[1][1] >= P11_before,
		     "P[1][1] should grow after predict");
}

/* ----------------------------------------------------------------
 * filter_update tests
 * ---------------------------------------------------------------- */

/**
 * @brief Test filter_update with NULL pointer.
 */
ZTEST(kalman_filter_tests, test_update_null)
{
	int ret = filter_update(NULL, 100.0f);
	zassert_equal(ret, -EINVAL, "filter_update(NULL) should return -EINVAL");
}

/**
 * @brief Test filter_update moves state toward measurement.
 *
 * After init, state[0]=0. Updating with z=100 should move altitude
 * toward 100.
 */
ZTEST(kalman_filter_tests, test_update_moves_toward_measurement)
{
	float alt_before = filt.state[0];

	int ret = filter_update(&filt, 100.0f);
	zassert_equal(ret, 0, "filter_update should return 0");

	zassert_true(filt.state[0] > alt_before,
		     "Altitude should increase toward measurement");
	zassert_true(filt.state[0] <= 100.0f,
		     "Altitude should not overshoot measurement");
}

/**
 * @brief Test filter_update reduces covariance.
 *
 * A measurement update should reduce uncertainty.
 */
ZTEST(kalman_filter_tests, test_update_reduces_covariance)
{
	float P00_before = filt.covariance[0][0];

	filter_update(&filt, 50.0f);

	zassert_true(filt.covariance[0][0] < P00_before,
		     "P[0][0] should decrease after measurement update");
}

/**
 * @brief Test filter converges to repeated measurements.
 *
 * Feed the same measurement repeatedly; the estimate should converge.
 */
ZTEST(kalman_filter_tests, test_update_convergence)
{
	const float measurement = 500.0f;

	for (int i = 0; i < 50; i++) {
		filter_predict(&filt, 10 * NS_PER_MS);
		filter_update(&filt, measurement);
	}

	zassert_near(filt.state[0], measurement, 5.0f,
		     "Altitude should converge to repeated measurement");
}

/* ----------------------------------------------------------------
 * filter_detect_apogee tests
 * ---------------------------------------------------------------- */

/**
 * @brief Test filter_detect_apogee with NULL pointer.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_null)
{
	int ret = filter_detect_apogee(NULL);
	zassert_equal(ret, -EINVAL,
		      "filter_detect_apogee(NULL) should return -EINVAL");
}

/**
 * @brief Test apogee not detected with zero velocity.
 *
 * Starting from zero velocity (no positive->non-positive crossing).
 */
ZTEST(kalman_filter_tests, test_detect_apogee_zero_velocity)
{
	filt.state[1] = 0.0f;

	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No apogee with initial zero velocity");
}

/**
 * @brief Test apogee not detected with positive velocity.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_positive_velocity)
{
	filt.state[1] = 10.0f;

	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No apogee with positive velocity");
}

/**
 * @brief Test apogee detected on velocity zero-crossing.
 *
 * Velocity goes from positive to zero/negative -> apogee.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_crossing)
{
	/* First call: set prev_velocity to positive */
	filt.state[1] = 10.0f;
	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No apogee yet while ascending");

	/* Second call: velocity crosses to zero -> apogee */
	filt.state[1] = 0.0f;
	ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 1, "Apogee should be detected on zero-crossing");
}

/**
 * @brief Test apogee detected on velocity crossing to negative.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_crossing_negative)
{
	filt.state[1] = 5.0f;
	filter_detect_apogee(&filt);

	filt.state[1] = -1.0f;
	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 1, "Apogee should be detected on crossing to negative");
}

/**
 * @brief Test no apogee detected when velocity stays negative.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_stays_negative)
{
	filt.state[1] = -5.0f;
	filter_detect_apogee(&filt);

	filt.state[1] = -10.0f;
	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No apogee when velocity stays negative");
}

/**
 * @brief Test no repeat apogee after crossing.
 *
 * After apogee is detected, subsequent calls with negative velocity
 * should not trigger again.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_no_repeat)
{
	filt.state[1] = 10.0f;
	filter_detect_apogee(&filt);

	filt.state[1] = -1.0f;
	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 1, "First crossing should detect apogee");

	filt.state[1] = -2.0f;
	ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No repeat apogee detection");
}

/* ----------------------------------------------------------------
 * Integration tests: predict + update cycle
 * ---------------------------------------------------------------- */

/**
 * @brief Test a simulated ascent and descent trajectory.
 *
 * Feeds rising altitude measurements, then falling ones at 100 ms
 * intervals (realistic barometric sample rate). Implied velocity
 * is 10 m / 0.1 s = 100 m/s. The filter velocity estimate should
 * transition from positive to negative, triggering apogee detection.
 */
ZTEST(kalman_filter_tests, test_flight_trajectory)
{
	int apogee_detected = 0;

	/* Ascent: altitude increasing */
	for (int i = 0; i < 30; i++) {
		float alt = 10.0f * i;  /* 0, 10, 20, ..., 290 */
		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_false(apogee_detected, "No apogee during ascent");

	/* Descent: altitude decreasing from peak */
	for (int i = 0; i < 30; i++) {
		float alt = 290.0f - 10.0f * i;  /* 290, 280, ..., 0 */
		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_true(apogee_detected,
		     "Apogee should be detected during descent");
}

/**
 * @brief Test predict-update cycle does not produce NaN or Inf.
 *
 * Run many cycles and verify state remains finite.
 */
ZTEST(kalman_filter_tests, test_numerical_stability)
{
	for (int i = 0; i < 200; i++) {
		float alt = 100.0f + 50.0f * sinf((float)i * 0.1f);
		filter_predict(&filt, 10 * NS_PER_MS);
		filter_update(&filt, alt);
	}

	zassert_true(isfinite(filt.state[0]), "Altitude should remain finite");
	zassert_true(isfinite(filt.state[1]), "Velocity should remain finite");
	zassert_true(isfinite(filt.covariance[0][0]), "P[0][0] should remain finite");
	zassert_true(isfinite(filt.covariance[1][1]), "P[1][1] should remain finite");
}
