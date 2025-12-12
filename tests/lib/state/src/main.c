/*
 * Copyright (c) 2025 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <lib/state/simple.h>

static const struct sm_thresholds simple_state_cfg_mock = {
	.T_D = 5.0,
	.T_A = 1.5,
	.T_H = 2.0,
	.T_Rd = 100.0,
	.T_Lh = 1.0,
	.T_La = 0.3,
	.T_L = 1000,
	.T_R = 2,
	.T_R2 = 5,
};

static void simple_state_machine_mock_before(void *fixture)
{
	sm_init(&simple_state_cfg_mock);
}

static void simple_state_machine_mock_after(void *fixture)
{
	sm_deinit();
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
	zassert_equal(sm_get_state(), SM_DISARMED, "Initial state should be DISARMED");
	sm_deinit();
	zassert_equal(sm_get_state(), SM_DISARMED, "Initial state should be DISARMED");
}

/**
 * @brief Test Simple State Update
 *
 * This test verifies the simple state update mechanism
 *
 */
ZTEST(simple_state_tests, test_states_update)
{
	struct sm_inputs inputs = {
		.orientation = 0.0,
		.acceleration = 0.0,
		.height = 0.0,
		.previous_height = 0.0,
	};

	/* test setup case */
	zassert_equal(sm_get_state(), SM_DISARMED, "Initial state should be DISARMED");

	/* test first update puts idle */
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_IDLE, "State should be IDLE");

	/* test update < T_L is still idle */
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_IDLE, "State should still be IDLE");

	/* test update > T_L is now detect liftoff awaiting */
	k_sleep(K_MSEC(simple_state_cfg_mock.T_L)); // Wait for idle timer to expire
	sm_update(&inputs);
	zassert_equal(sm_get_state(), SM_DETECT_LIFTOFF_AWAITING, "State should be DETECT_LIFTOFF_AWAITING");
}
