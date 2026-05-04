/**
 * @file main.c
 * @brief Unit tests for the state machine shell commands.
 *
 * Uses the Zephyr dummy shell backend to execute commands and
 * verify return codes and output.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include <aurora/lib/state/state.h>
#include <aurora/lib/state/audit.h>

/* Build an orientation vector (yaw, pitch, roll) whose up-axis elevation
 * equals @p elev degrees.
 */
#define ORIENT(elev) { 0.0, 90.0 - (double)(elev), 0.0 }

/** @brief Test threshold configuration with fast timers for unit testing. */
static const struct sm_thresholds test_cfg = {
	.T_AB  = 30,
	.T_H   = 50,
	.T_BB  = 15,
	.T_M   = 200,
	.T_L   = 2,
	.T_OA  = 85,
	.T_OI  = 70,
	.N_OI  = 1,
	.DT_AB = 100,
	.DT_L  = 50,
	.TO_A  = 200,
	.TO_M  = 150,
	.TO_R  = 250,
};

static const struct shell *sh;

/**
 * @brief Execute a shell command and check that the output contains
 *        the given substring.
 *
 * Clears the dummy backend buffer, runs the command, and asserts
 * that the output includes @p expected.
 */
static void execute_and_check(const char *cmd, const char *expected)
{
	size_t size;
	const char *buf;
	int err;

	shell_backend_dummy_clear_output(sh);
	err = shell_execute_cmd(sh, cmd);
	zassert_ok(err, "command \"%s\" failed (err %d)", cmd, err);

	buf = shell_backend_dummy_get_output(sh, &size);
	zassert_true(size > 0,
		     "Shell produced no output for \"%s\"", cmd);
	zassert_not_null(strstr(buf, expected),
			 "Expected \"%s\" in output of \"%s\", got:\n%s",
			 expected, cmd, buf);
}

/*-----------------------------------------------------------
 * Fixtures
 *----------------------------------------------------------*/
static void state_shell_before(void *fixture)
{
	ARG_UNUSED(fixture);

	sm_init(&test_cfg, NULL);
	sm_audit_clear();
}

static void state_shell_after(void *fixture)
{
	ARG_UNUSED(fixture);

	sm_deinit();
}

static void *state_shell_setup(void)
{
	sh = shell_backend_dummy_get_ptr();

	/* Wait for the initialization of the shell dummy backend. */
	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "timed out waiting for dummy shell backend");

	return NULL;
}

ZTEST_SUITE(state_shell_tests, NULL, state_shell_setup,
	    state_shell_before, state_shell_after, NULL);

/*-----------------------------------------------------------
 * status command
 *----------------------------------------------------------*/

/**
 * @brief Test that "state_machine status" returns success and shows
 *        the state machine type and current state.
 */
ZTEST(state_shell_tests, test_status_shows_type_and_state)
{
	execute_and_check("state_machine status", "simple");
}

/**
 * @brief Test that "state_machine status" reflects state changes.
 */
ZTEST(state_shell_tests, test_status_reflects_armed)
{
	struct sm_inputs in = {
		.armed = 1,
		.orientation = ORIENT(test_cfg.T_OA),
	};

	sm_update(&in);
	zassert_equal(sm_get_state(), SM_ARMED, "Precondition: should be ARMED");

	execute_and_check("state_machine status", "ARMED");
}

/*-----------------------------------------------------------
 * transition command
 *----------------------------------------------------------*/

/**
 * @brief Test that "state_machine transition IDLE" succeeds and
 *        leaves the state machine in IDLE.
 */
ZTEST(state_shell_tests, test_transition_to_idle)
{
	int err;
	struct sm_inputs in = {
		.armed = 1,
		.orientation = ORIENT(test_cfg.T_OA),
	};

	/* Move to ARMED first */
	sm_update(&in);
	zassert_equal(sm_get_state(), SM_ARMED, "Precondition: should be ARMED");

	err = shell_execute_cmd(sh, "state_machine transition IDLE");
	zassert_ok(err, "transition command failed (err %d)", err);
	zassert_equal(sm_get_state(), SM_IDLE, "State should be IDLE after transition");
}

/**
 * @brief Test that "state_machine transition" with an unknown state
 *        returns an error.
 */
ZTEST(state_shell_tests, test_transition_unknown_state)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine transition BOGUS");
	zassert_not_equal(err, 0, "Should fail for unknown state");
}

/**
 * @brief Test that "state_machine transition" without arguments fails.
 */
ZTEST(state_shell_tests, test_transition_missing_arg)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine transition");
	zassert_not_equal(err, 0, "Should fail without state argument");
}

/**
 * @brief Test transition to current state prints a warning.
 */
ZTEST(state_shell_tests, test_transition_same_state)
{
	zassert_equal(sm_get_state(), SM_IDLE, "Precondition: should be IDLE");

	execute_and_check("state_machine transition IDLE", "Already in");
}

/*-----------------------------------------------------------
 * audit command
 *----------------------------------------------------------*/

/**
 * @brief Test that "state_machine audit" shows empty log when no
 *        transitions have occurred.
 */
ZTEST(state_shell_tests, test_audit_empty)
{
	execute_and_check("state_machine audit", "empty");
}

/**
 * @brief Test that audit log records a state transition.
 */
ZTEST(state_shell_tests, test_audit_records_transition)
{
	struct sm_inputs in = {
		.armed = 1,
		.orientation = ORIENT(test_cfg.T_OA),
	};

	/* Trigger IDLE -> ARMED transition */
	sm_update(&in);
	zassert_equal(sm_get_state(), SM_ARMED, "Precondition: should be ARMED");
	zassert_true(sm_audit_count() > 0, "Should have audit entries");

	execute_and_check("state_machine audit", "transition");
}

/**
 * @brief Test that audit log records events.
 */
ZTEST(state_shell_tests, test_audit_records_event)
{
	/* Record an event manually (sm_init event was cleared by fixture) */
	sm_audit_event(SM_IDLE, "test event");
	zassert_true(sm_audit_count() > 0, "Should have audit entries");

	execute_and_check("state_machine audit", "event");
}

/**
 * @brief Test that multiple transitions appear in audit log.
 */
ZTEST(state_shell_tests, test_audit_multiple_transitions)
{
	struct sm_inputs in = {
		.armed = 1,
		.orientation = ORIENT(test_cfg.T_OA),
	};

	/* IDLE -> ARMED */
	sm_update(&in);
	zassert_equal(sm_get_state(), SM_ARMED, "Should be ARMED");

	/* ARMED -> IDLE (disarm) */
	in.armed = 0;
	sm_update(&in);
	zassert_equal(sm_get_state(), SM_IDLE, "Should be IDLE");

	zassert_true(sm_audit_count() >= 2, "Should have at least 2 audit entries");

	execute_and_check("state_machine audit", "ARMED");
}

/*-----------------------------------------------------------
 * audit_clear command
 *----------------------------------------------------------*/

/**
 * @brief Test that "state_machine audit_clear" empties the log.
 */
ZTEST(state_shell_tests, test_audit_clear)
{
	int err;
	struct sm_inputs in = {
		.armed = 1,
		.orientation = ORIENT(test_cfg.T_OA),
	};

	/* Generate some audit entries */
	sm_update(&in);
	zassert_true(sm_audit_count() > 0, "Should have audit entries");

	err = shell_execute_cmd(sh, "state_machine audit_clear");
	zassert_ok(err, "audit_clear command failed (err %d)", err);
	zassert_equal(sm_audit_count(), 0, "Audit log should be empty after clear");

	/* Verify audit command now shows empty */
	execute_and_check("state_machine audit", "empty");
}
