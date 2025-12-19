/*
 * Copyright (c) 2025 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <lib/state/simple.h>

static const struct sm_thresholds simple_state_cfg = {
	/* Sensor Metrics */
	.T_AB = 30.0f,	// 30 m/s^2
	.T_H = 50.0f,	// 50 m
	.T_BB = 15.0f,	// 15 m/s^2
	.T_M = 200.0f,	// 200 m
	.T_L = 2.0f,	// 2 m/s
	.T_OA = 85.0f,	// 85 degrees
	.T_OI = 70.0f,	// 70 degrees

	/* Timers */
	.DT_AB = 100,	// 100 ms
	.DT_L = 50,		// 50 ms

	/* Timeouts */
	.TO_A = 200,	// 200 ms
	.TO_M = 150,	// 150 ms
	.TO_R = 250,	// 250 ms
};

static void simple_state_machine_mock_before(void *fixture)
{
	sm_init(&simple_state_cfg);
}

static void simple_state_machine_mock_after(void *fixture)
{
	sm_deinit();
}

static inline void put_state_armed(struct sm_inputs *in)
{
	in->armed = 0;
	sm_update(in);
	zassert_equal(sm_get_state(), SM_IDLE, "State should be disarmed to IDLE");

	in->armed = 1;
	in->orientation = simple_state_cfg.T_OA;
	sm_update(in);
	zassert_equal(sm_get_state(), SM_ARMED, "State should be ARMED");
}

ZTEST_SUITE(simple_state_tests, NULL, NULL, simple_state_machine_mock_before, simple_state_machine_mock_after, NULL);

/**
 * @brief Test Simple State Setup
 *
 * This test verifies the simple state setup mechanism
 *
 */
ZTEST(simple_state_tests, test_states_setup)
{
	zassert_equal(sm_get_state(), SM_IDLE, "Initial state should be DISARMED");
	sm_deinit();
	zassert_equal(sm_get_state(), SM_IDLE, "Initial state should be DISARMED");
}

/**
 * @brief Test Simple State Updates in IDLE
 *
 * This test verifies the simple state update mechanism in IDLE
 *
 */
ZTEST(simple_state_tests, test_state_idle)
{
	struct sm_inputs inputs = {
		.armed = 0,
		.orientation = 0.0,
		.acceleration = 0.0,
		.velocity = 0.0,
		.altitude = 0.0,
	};

	/* test setup case */
	zassert_equal(sm_get_state(), SM_IDLE, "Initial state should be IDLE");

	/* test first update is still idle */
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_IDLE, "State should still be IDLE");

	/* test update ARM without orientation */
	inputs.armed = 1;
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_IDLE, "State should still be IDLE");

	/* test update orientation without arm */
	inputs.armed = 0;
	inputs.orientation = simple_state_cfg.T_OA;
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_IDLE, "State should still be IDLE");

	/* test update orientation and arm */
	put_state_armed(&inputs);
}

/**
 * @brief Test Simple State Updates in ARMED
 *
 * This test verifies the simple state update mechanism in ARMED
 *
 */
ZTEST(simple_state_tests, test_state_armed)
{
	struct sm_inputs inputs = {
		.armed = 0,
		.orientation = 0.0,
		.acceleration = 0.0,
		.velocity = 0.0,
		.altitude = 0.0,
	};

	/* test setup case */
	zassert_equal(sm_get_state(), SM_IDLE, "Initial state should be IDLE");

	/* bring to armed */
	put_state_armed(&inputs);

	/* test update disarm */
	inputs.armed = 0.0;
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_IDLE, "State should now be IDLE");

	/* test update disarm via orientation */
	put_state_armed(&inputs);

	inputs.orientation = simple_state_cfg.T_OI - 1.0f;
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_IDLE, "State should now be IDLE");

	/* test update to boost (fail -> accel) */
	put_state_armed(&inputs);

	inputs.acceleration = simple_state_cfg.T_AB - 1.0f;
	inputs.altitude = simple_state_cfg.T_H;
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_ARMED, "State should still be ARMED");

	k_sleep(K_MSEC(simple_state_cfg.DT_AB));
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_ARMED, "State should still be ARMED");

	/* test update to boost (fail -> altitude) */
	put_state_armed(&inputs);

	inputs.acceleration = simple_state_cfg.T_AB;
	inputs.altitude = simple_state_cfg.T_H - 1.0f;
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_ARMED, "State should still be ARMED");

	k_sleep(K_MSEC(simple_state_cfg.DT_AB));
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_ARMED, "State should still be ARMED");

	/* test update to boost */
	put_state_armed(&inputs);

	inputs.acceleration = simple_state_cfg.T_AB;
	inputs.altitude = simple_state_cfg.T_H;
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_ARMED, "State should still be ARMED");
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_ARMED, "State should still be ARMED");

	k_sleep(K_MSEC(simple_state_cfg.DT_AB));
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_BOOST, "State should now be BOOST");
}
