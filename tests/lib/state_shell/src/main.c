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
#include <aurora/lib/state/config.h>

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
		.log_ready = 1,
		.calibrated = 1,
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
		.log_ready = 1,
		.calibrated = 1,
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

/**
 * @brief Test that "state_machine transition CALIBRATING" is rejected.
 *
 * CALIBRATING is no longer a valid state name (calibration now runs
 * within IDLE, gated by the calibrated input flag rather than a
 * dedicated state) -- the shell must reject it as unknown, same as any
 * other bogus state name.
 */
ZTEST(state_shell_tests, test_transition_calibrating_rejected)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine transition CALIBRATING");
	zassert_not_equal(err, 0, "CALIBRATING should no longer be a valid state name");
}

/*-----------------------------------------------------------
 * config command
 *
 * This build has no threshold store, so "set" and "default" apply the new
 * values and warn that they will not survive a reboot; the persistence
 * itself is covered by the state_config suite.
 *----------------------------------------------------------*/

/** @brief Read one threshold out of the running set. */
static int running_threshold(const char *name)
{
	struct sm_thresholds cur;

	sm_backend_get_thresholds(&cur);

	return sm_config_field_get(&cur, sm_config_field_find(name));
}

/**
 * @brief Test that "state_machine config" lists the running thresholds.
 */
ZTEST(state_shell_tests, test_config_lists_thresholds)
{
	execute_and_check("state_machine config", "T_AB");
	execute_and_check("state_machine config", "TO_R");
}

/**
 * @brief Test that "state_machine config set" applies the new value.
 */
ZTEST(state_shell_tests, test_config_set_applies)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine config set T_M 400");
	zassert_ok(err, "config set failed (err %d)", err);
	zassert_equal(running_threshold("T_M"), 400, "T_M should have been applied");
}

/**
 * @brief Test that "state_machine config set" accepts lowercase names.
 */
ZTEST(state_shell_tests, test_config_set_is_case_insensitive)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine config set t_m 350");
	zassert_ok(err, "config set failed (err %d)", err);
	zassert_equal(running_threshold("T_M"), 350, "T_M should have been applied");
}

/**
 * @brief Test that an unknown threshold name is rejected.
 */
ZTEST(state_shell_tests, test_config_set_unknown_name)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine config set T_BOGUS 1");
	zassert_not_equal(err, 0, "Should fail for an unknown threshold");
}

/**
 * @brief Test that a non-numeric value is rejected.
 */
ZTEST(state_shell_tests, test_config_set_non_numeric_value)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine config set T_M high");
	zassert_not_equal(err, 0, "Should fail for a non-numeric value");
	zassert_equal(running_threshold("T_M"), test_cfg.T_M, "T_M should be unchanged");
}

/**
 * @brief Test that a value outside the threshold's range is rejected.
 */
ZTEST(state_shell_tests, test_config_set_out_of_range)
{
	int err;

	/* T_OA is an elevation, capped at 90 degrees. */
	err = shell_execute_cmd(sh, "state_machine config set T_OA 500");
	zassert_not_equal(err, 0, "Should fail for an out-of-range value");
	zassert_equal(running_threshold("T_OA"), test_cfg.T_OA, "T_OA should be unchanged");
}

/**
 * @brief Test that "state_machine config set" is refused outside IDLE.
 *
 * Swapping a threshold mid-flight would race the timers already started
 * under the old set, so the shell must refuse rather than apply it.
 */
ZTEST(state_shell_tests, test_config_set_refused_outside_idle)
{
	int err;
	struct sm_inputs in = {
		.armed = 1,
		.log_ready = 1,
		.calibrated = 1,
		.orientation = ORIENT(test_cfg.T_OA),
	};

	sm_update(&in);
	zassert_equal(sm_get_state(), SM_ARMED, "Precondition: should be ARMED");

	err = shell_execute_cmd(sh, "state_machine config set T_M 400");
	zassert_not_equal(err, 0, "Should fail outside IDLE");
	zassert_equal(running_threshold("T_M"), test_cfg.T_M, "T_M should be unchanged");
}

/**
 * @brief Test that "state_machine config default" restores the Kconfig set.
 */
ZTEST(state_shell_tests, test_config_default_restores_kconfig)
{
	struct sm_thresholds def, cur;
	int err;

	sm_config_defaults(&def);

	err = shell_execute_cmd(sh, "state_machine config set T_M 400");
	zassert_ok(err, "config set failed (err %d)", err);

	err = shell_execute_cmd(sh, "state_machine config default");
	zassert_ok(err, "config default failed (err %d)", err);

	sm_backend_get_thresholds(&cur);
	zassert_mem_equal(&cur, &def, sizeof(cur),
			  "config default should restore the whole Kconfig set");
}

/**
 * @brief Test that "state_machine config save" reports a missing store.
 */
ZTEST(state_shell_tests, test_config_save_without_store)
{
	int err;

	err = shell_execute_cmd(sh, "state_machine config save");
	zassert_not_equal(err, 0, "Should fail without a threshold store");
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
		.log_ready = 1,
		.calibrated = 1,
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
		.log_ready = 1,
		.calibrated = 1,
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
		.log_ready = 1,
		.calibrated = 1,
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
