/**
 * @file main.c
 * @brief Unit tests for the Kalman filter apogee detection with
 *        hypsometric altitude conversion.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <zephyr/ztest.h>
#include <aurora/lib/filter.h>

/** @brief Absolute tolerance for floating-point comparisons. */
#define FLOAT_TOL 0.001

/** @brief Helper: assert two doubles are approximately equal. */
#define zassert_near(a, b, tol, msg) \
	zassert_true(fabs((a) - (b)) < (tol), msg)

/**
 * @brief Expected noise values from Kconfig defaults.
 *
 * CONFIG_FILTER_Q_ALT_MILLISCALE = 100  -> 0.1
 * CONFIG_FILTER_Q_VEL_MILLISCALE = 500  -> 0.5
 * CONFIG_FILTER_R_MILLISCALE     = 4000 -> 4.0
 */
#define EXPECTED_Q_ALT (100.0 / 1000.0)
#define EXPECTED_Q_VEL (500.0 / 1000.0)
#define EXPECTED_R     (4000.0 / 1000.0)

/** @brief Nanoseconds per millisecond for dt conversion. */
#define NS_PER_MS 1000000LL

/* ----------------------------------------------------------------
 * ISA troposphere constants (mirror aurora/lib/sensor/baro.c)
 * ---------------------------------------------------------------- */

/** @brief ISA sea-level temperature (K). */
#define ISA_T0 288.15

/** @brief ISA temperature lapse rate (K/m). */
#define ISA_L  0.0065

/** @brief g*M / (R*L) exponent for the barometric formula. */
#define ISA_GMR_OVER_L 5.25588

/** @brief R*L / (g*M) exponent for the hypsometric formula. */
#define ISA_RL_OVER_GM 0.190263

/** @brief Standard sea-level pressure (kPa). */
#define ISA_P0_KPA 101.325

/**
 * @brief Convert altitude to pressure using the barometric formula.
 *
 * P = P_ref * (1 - L*h / T0) ^ (g*M / (R*L))
 *
 * @param alt_m    Altitude AGL in meters.
 * @param ref_kpa  Reference (ground-level) pressure in kPa.
 * @return Pressure in kPa.
 */
static double altitude_to_pressure(double alt_m, double ref_kpa)
{
	return ref_kpa * pow(1.0 - ISA_L * alt_m / ISA_T0, ISA_GMR_OVER_L);
}

/**
 * @brief Convert pressure to altitude using the hypsometric formula.
 *
 * h = (T0 / L) * (1 - (P / P_ref) ^ (R*L / (g*M)))
 *
 * This mirrors baro_pressure_to_altitude() from baro.c.
 *
 * @param press_kpa  Measured pressure in kPa.
 * @param ref_kpa    Reference (ground-level) pressure in kPa.
 * @return Altitude AGL in meters.
 */
static double pressure_to_altitude(double press_kpa, double ref_kpa)
{
	return (ISA_T0 / ISA_L) *
	       (1.0 - pow(press_kpa / ref_kpa, ISA_RL_OVER_GM));
}

static struct filter filt;

static void kalman_filter_before(void *fixture)
{
	filter_init(&filt);
}

/* ================================================================
 * Suite 1: Filter core unit tests
 * ================================================================ */

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
	zassert_near(f.state[0], 0.0, FLOAT_TOL, "Initial altitude should be 0");
	zassert_near(f.state[1], 0.0, FLOAT_TOL, "Initial velocity should be 0");

	/* Covariance diagonal = 10, off-diagonal = 0 */
	zassert_near(f.covariance[0][0], 10.0, FLOAT_TOL, "P[0][0] should be 10");
	zassert_near(f.covariance[0][1], 0.0, FLOAT_TOL, "P[0][1] should be 0");
	zassert_near(f.covariance[1][0], 0.0, FLOAT_TOL, "P[1][0] should be 0");
	zassert_near(f.covariance[1][1], 10.0, FLOAT_TOL, "P[1][1] should be 10");

	/* Process noise from Kconfig defaults */
	zassert_near(f.noise_p[0][0], EXPECTED_Q_ALT, FLOAT_TOL,
		     "Q_alt should match Kconfig");
	zassert_near(f.noise_p[0][1], 0.0, FLOAT_TOL, "Q[0][1] should be 0");
	zassert_near(f.noise_p[1][0], 0.0, FLOAT_TOL, "Q[1][0] should be 0");
	zassert_near(f.noise_p[1][1], EXPECTED_Q_VEL, FLOAT_TOL,
		     "Q_vel should match Kconfig");

	/* Measurement noise from Kconfig */
	zassert_near(f.noise_m, EXPECTED_R, FLOAT_TOL,
		     "R should match Kconfig");

	/* Previous velocity for apogee detection */
	zassert_near(f.prev_velocity, 0.0, FLOAT_TOL,
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
	filt.state[0] = 100.0;  /* altitude */
	filt.state[1] = 50.0;   /* velocity */

	/* dt = 100ms = 100,000,000 ns -> dt_s = 0.1 */
	int ret = filter_predict(&filt, 100 * NS_PER_MS);
	zassert_equal(ret, 0, "filter_predict should return 0");

	zassert_near(filt.state[0], 105.0, FLOAT_TOL,
		     "Altitude should be predicted forward by velocity * dt_s");
	zassert_near(filt.state[1], 50.0, FLOAT_TOL,
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
	filt.state[0] = 100.0;
	filt.state[1] = 50.0;

	int ret = filter_predict(&filt, 500000LL);  /* 0.5 ms */
	zassert_equal(ret, 0, "filter_predict should return 0");

	zassert_near(filt.state[0], 100.025, FLOAT_TOL,
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
	double P00_before = filt.covariance[0][0];
	double P11_before = filt.covariance[1][1];

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
	int ret = filter_update(NULL, 100.0);
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
	double alt_before = filt.state[0];

	int ret = filter_update(&filt, 100.0);
	zassert_equal(ret, 0, "filter_update should return 0");

	zassert_true(filt.state[0] > alt_before,
		     "Altitude should increase toward measurement");
	zassert_true(filt.state[0] <= 100.0,
		     "Altitude should not overshoot measurement");
}

/**
 * @brief Test filter_update reduces covariance.
 *
 * A measurement update should reduce uncertainty.
 */
ZTEST(kalman_filter_tests, test_update_reduces_covariance)
{
	double P00_before = filt.covariance[0][0];

	filter_update(&filt, 50.0);

	zassert_true(filt.covariance[0][0] < P00_before,
		     "P[0][0] should decrease after measurement update");
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
	filt.state[1] = 0.0;

	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No apogee with initial zero velocity");
}

/**
 * @brief Test apogee not detected with positive velocity.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_positive_velocity)
{
	filt.state[1] = 10.0;

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
	filt.state[1] = 10.0;
	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No apogee yet while ascending");

	/* Second call: velocity crosses to zero -> apogee */
	filt.state[1] = 0.0;
	ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 1, "Apogee should be detected on zero-crossing");
}

/**
 * @brief Test apogee detected on velocity crossing to negative.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_crossing_negative)
{
	filt.state[1] = 5.0;
	filter_detect_apogee(&filt);

	filt.state[1] = -1.0;
	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 1, "Apogee should be detected on crossing to negative");
}

/**
 * @brief Test no apogee detected when velocity stays negative.
 */
ZTEST(kalman_filter_tests, test_detect_apogee_stays_negative)
{
	filt.state[1] = -5.0;
	filter_detect_apogee(&filt);

	filt.state[1] = -10.0;
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
	filt.state[1] = 10.0;
	filter_detect_apogee(&filt);

	filt.state[1] = -1.0;
	int ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 1, "First crossing should detect apogee");

	filt.state[1] = -2.0;
	ret = filter_detect_apogee(&filt);
	zassert_equal(ret, 0, "No repeat apogee detection");
}

/* ================================================================
 * Suite 2: Hypsometric altitude pipeline tests
 *
 * These tests verify that the Kalman filter works correctly when
 * fed altitude measurements derived from pressure via the ISA
 * hypsometric formula, matching the real sensor pipeline.
 * ================================================================ */

ZTEST_SUITE(hypsometric_pipeline_tests, NULL, NULL, kalman_filter_before,
	    NULL, NULL);

/* ----------------------------------------------------------------
 * Hypsometric conversion sanity checks
 * ---------------------------------------------------------------- */

/**
 * @brief Verify pressure-to-altitude round-trip at ground level.
 *
 * At reference pressure the hypsometric formula must return 0 m.
 */
ZTEST(hypsometric_pipeline_tests, test_hypsometric_ground_level)
{
	double alt = pressure_to_altitude(ISA_P0_KPA, ISA_P0_KPA);

	zassert_near(alt, 0.0, FLOAT_TOL,
		     "Altitude at reference pressure should be 0 m");
}

/**
 * @brief Verify altitude-to-pressure-to-altitude round-trip.
 *
 * Convert 500 m -> pressure -> altitude and check we get 500 m back.
 */
ZTEST(hypsometric_pipeline_tests, test_hypsometric_round_trip)
{
	const double target_alt = 500.0;
	double press = altitude_to_pressure(target_alt, ISA_P0_KPA);
	double recovered_alt = pressure_to_altitude(press, ISA_P0_KPA);

	zassert_near(recovered_alt, target_alt, 0.1,
		     "Round-trip altitude should match within 0.1 m");
}

/**
 * @brief Verify hypsometric formula at a known ISA altitude.
 *
 * At 1000 m ISA, pressure is approximately 89.875 kPa.
 */
ZTEST(hypsometric_pipeline_tests, test_hypsometric_known_altitude)
{
	const double alt_1000m = 1000.0;
	double press = altitude_to_pressure(alt_1000m, ISA_P0_KPA);

	/* ISA standard: ~89.875 kPa at 1000 m */
	zassert_near(press, 89.875, 0.1,
		     "Pressure at 1000 m should be ~89.875 kPa");

	double alt = pressure_to_altitude(press, ISA_P0_KPA);

	zassert_near(alt, alt_1000m, 0.1,
		     "Recovered altitude should be ~1000 m");
}

/* ----------------------------------------------------------------
 * Filter convergence with hypsometric input
 * ---------------------------------------------------------------- */

/**
 * @brief Test filter converges when fed repeated hypsometric altitude.
 *
 * Converts a constant pressure (corresponding to ~500 m) to altitude
 * via the hypsometric formula, then feeds it to the filter repeatedly.
 * The estimate should converge to that altitude.
 */
ZTEST(hypsometric_pipeline_tests, test_convergence_from_pressure)
{
	const double target_alt = 500.0;
	const double press = altitude_to_pressure(target_alt, ISA_P0_KPA);
	const double hyp_alt = pressure_to_altitude(press, ISA_P0_KPA);

	for (int i = 0; i < 50; i++) {
		filter_predict(&filt, 10 * NS_PER_MS);
		filter_update(&filt, hyp_alt);
	}

	zassert_near(filt.state[0], target_alt, 5.0,
		     "Filter should converge to hypsometric altitude");
}

/* ----------------------------------------------------------------
 * Full flight trajectory with hypsometric pipeline
 * ---------------------------------------------------------------- */

/**
 * @brief Simulated flight trajectory using pressure-derived altitudes.
 *
 * Models a rocket flight profile:
 *   - Ground reference at standard sea-level pressure (101.325 kPa)
 *   - Ascent:  altitude 0 -> 300 m (pressure decreasing)
 *   - Descent: altitude 300 -> 0 m (pressure increasing)
 *
 * Pressures are converted to altitude via the hypsometric formula
 * before being fed to the Kalman filter, matching the real pipeline.
 * The filter should detect apogee during the descent phase.
 */
ZTEST(hypsometric_pipeline_tests, test_flight_trajectory_hypsometric)
{
	const double ref_kpa = ISA_P0_KPA;
	int apogee_detected = 0;

	/* Ascent: altitude 0 -> 290 m in 10 m steps */
	for (int i = 0; i < 30; i++) {
		double true_alt = 10.0 * i;
		double press = altitude_to_pressure(true_alt, ref_kpa);
		double meas_alt = pressure_to_altitude(press, ref_kpa);

		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, meas_alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_false(apogee_detected, "No apogee during ascent");

	/* Descent: altitude 290 -> 0 m in 10 m steps */
	for (int i = 0; i < 30; i++) {
		double true_alt = 290.0 - 10.0 * i;
		double press = altitude_to_pressure(true_alt, ref_kpa);
		double meas_alt = pressure_to_altitude(press, ref_kpa);

		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, meas_alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_true(apogee_detected,
		     "Apogee should be detected during descent");
}

/**
 * @brief Flight trajectory with non-standard ground pressure.
 *
 * Verifies the pipeline works correctly when the launch site is not
 * at sea level (e.g., 95 kPa reference, ~540 m elevation).
 */
ZTEST(hypsometric_pipeline_tests, test_flight_trajectory_high_site)
{
	const double ref_kpa = 95.0;
	int apogee_detected = 0;

	/* Ascent: 0 -> 290 m AGL */
	for (int i = 0; i < 30; i++) {
		double true_alt = 10.0 * i;
		double press = altitude_to_pressure(true_alt, ref_kpa);
		double meas_alt = pressure_to_altitude(press, ref_kpa);

		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, meas_alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_false(apogee_detected, "No apogee during ascent");

	/* Descent: 290 -> 0 m AGL */
	for (int i = 0; i < 30; i++) {
		double true_alt = 290.0 - 10.0 * i;
		double press = altitude_to_pressure(true_alt, ref_kpa);
		double meas_alt = pressure_to_altitude(press, ref_kpa);

		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, meas_alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_true(apogee_detected,
		     "Apogee should be detected at high-elevation site");
}

/**
 * @brief Flight trajectory with simulated barometric noise.
 *
 * Adds deterministic pseudo-noise to pressure readings to simulate
 * MS5607 sensor noise (~0.05 kPa RMS at 4096 OSR).  The Kalman
 * filter should still detect apogee despite noisy measurements.
 */
ZTEST(hypsometric_pipeline_tests, test_flight_trajectory_noisy)
{
	const double ref_kpa = ISA_P0_KPA;
	const double noise_amp_kpa = 0.05;
	int apogee_detected = 0;

	/* Ascent: 0 -> 490 m in 10 m steps */
	for (int i = 0; i < 50; i++) {
		double true_alt = 10.0 * i;
		double press = altitude_to_pressure(true_alt, ref_kpa);
		/* Deterministic pseudo-noise via sin */
		double noisy_press = press +
			noise_amp_kpa * sin((double)i * 1.7);
		double meas_alt = pressure_to_altitude(noisy_press, ref_kpa);

		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, meas_alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_false(apogee_detected,
		      "No apogee during noisy ascent");

	/* Descent: 490 -> 0 m in 10 m steps */
	for (int i = 0; i < 50; i++) {
		double true_alt = 490.0 - 10.0 * i;
		double press = altitude_to_pressure(true_alt, ref_kpa);
		double noisy_press = press +
			noise_amp_kpa * sin((double)(i + 50) * 1.7);
		double meas_alt = pressure_to_altitude(noisy_press, ref_kpa);

		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, meas_alt);
		if (filter_detect_apogee(&filt) == 1) {
			apogee_detected = 1;
		}
	}
	zassert_true(apogee_detected,
		     "Apogee should be detected despite noisy pressure");
}

/* ----------------------------------------------------------------
 * Numerical stability with hypsometric input
 * ---------------------------------------------------------------- */

/**
 * @brief Test predict-update cycle does not produce NaN or Inf
 *        when fed pressure-derived altitude oscillations.
 *
 * Simulates pressure varying sinusoidally around a nominal altitude,
 * converts to altitude via the hypsometric formula, and feeds to the
 * filter for 200 cycles.
 */
ZTEST(hypsometric_pipeline_tests, test_numerical_stability_hypsometric)
{
	const double ref_kpa = ISA_P0_KPA;
	const double nominal_alt = 100.0;

	for (int i = 0; i < 200; i++) {
		double alt_var = nominal_alt +
			50.0 * sin(i * 0.1);
		double press = altitude_to_pressure(alt_var, ref_kpa);
		double meas_alt = pressure_to_altitude(press, ref_kpa);

		filter_predict(&filt, 10 * NS_PER_MS);
		filter_update(&filt, meas_alt);
	}

	zassert_true(isfinite(filt.state[0]),
		     "Altitude should remain finite");
	zassert_true(isfinite(filt.state[1]),
		     "Velocity should remain finite");
	zassert_true(isfinite(filt.covariance[0][0]),
		     "P[0][0] should remain finite");
	zassert_true(isfinite(filt.covariance[1][1]),
		     "P[1][1] should remain finite");
}

/**
 * @brief Verify filter altitude estimate tracks hypsometric input.
 *
 * After feeding a steady 200 m altitude (from pressure) for many
 * cycles, the filter estimate should be close to 200 m and velocity
 * should be near zero.
 */
ZTEST(hypsometric_pipeline_tests, test_steady_state_tracking)
{
	const double ref_kpa = ISA_P0_KPA;
	const double target_alt = 200.0;
	const double press = altitude_to_pressure(target_alt, ref_kpa);
	const double meas_alt = pressure_to_altitude(press, ref_kpa);

	for (int i = 0; i < 100; i++) {
		filter_predict(&filt, 100 * NS_PER_MS);
		filter_update(&filt, meas_alt);
	}

	zassert_near(filt.state[0], target_alt, 1.0,
		     "Filter altitude should track steady input");
	zassert_near(filt.state[1], 0.0, 1.0,
		     "Filter velocity should be near zero at steady state");
}
