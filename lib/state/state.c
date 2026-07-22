/**
 * @file state.c
 * @brief Common core for the AURORA flight state machine.
 *
 * Implements the public API declared in state.h that is independent of any
 * particular state machine implementation: lifecycle (init/deinit), the
 * update entry point and its input filtering, the current-state accessor,
 * and the error-handling machinery.  The backend-specific transition logic
 * lives in a separate backend translation unit (e.g. simple.c) selected at
 * build time; this core drives it through the sm_backend_* hooks and the
 * backend drives this core back through sm_transition() / sm_event() /
 * sm_do_error_handling().
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <aurora/lib/state/state.h>
#include "state_internal.h"

#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)
#include <aurora/lib/state/audit.h>
#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */

#if defined(CONFIG_FILTER)
#include <aurora/lib/filter.h>
static struct filter filter;
#endif /* CONFIG_FILTER */

LOG_MODULE_REGISTER(state_machine, CONFIG_STATE_MACHINE_LOG_LEVEL);

/*-----------------------------------------------------------
 * Prototypes
 *----------------------------------------------------------*/

/**
 * @brief Default error handler when no user callback is registered.
 *
 * @param reason Why the state machine entered SM_ERROR.
 * @param args Unused opaque argument (always NULL).
 * @return Always returns -EIO.
 */
static int fallback_sm_error_handler(enum sm_error_reason reason, void *args);

/*-----------------------------------------------------------
 * Internal State
 *----------------------------------------------------------*/
static enum sm_state current_state = SM_IDLE; /**< Active flight state. */
static struct sm_inputs last_inputs; /**< Last inputs evaluated by the backend. */

static struct k_spinlock err_lock; /**< Spinlock protecting error callback invocation. */
static struct sm_error_handling_args err_hdl = {
	.cb = &fallback_sm_error_handler,
	.args = NULL,
};

/* Why the machine is (or last was) in SM_ERROR.  Recorded by
 * sm_do_error_handling() so re-invocations from the SM_ERROR state keep
 * reporting the original cause to the callback.
 */
static enum sm_error_reason err_reason = SM_ERR_UNKNOWN;

/*-----------------------------------------------------------
 * Transition / audit helpers (see state_internal.h)
 *----------------------------------------------------------*/

/* sm_transition – see state_internal.h */
void sm_transition(enum sm_state new_state)
{
#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)
	sm_audit_transition(current_state, new_state);
#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */
	current_state = new_state;
}

/* sm_event – see state_internal.h */
void sm_event(const char *msg)
{
#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)
	sm_audit_event(current_state, msg);
#else
	ARG_UNUSED(msg);
#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */
}

/*-----------------------------------------------------------
 * Error handling
 *----------------------------------------------------------*/

static int fallback_sm_error_handler(enum sm_error_reason reason, void *args)
{
	ARG_UNUSED(args);
	LOG_ERR("State Machine encountered an unrecoverable error (%s)",
		sm_error_reason_str(reason));

	return -EIO;
}

/* sm_do_error_handling – see state_internal.h */
void sm_do_error_handling(enum sm_error_reason reason)
{
	int ret;
	k_spinlock_key_t key = k_spin_lock(&err_lock);

	if (current_state != SM_ERROR) {
		err_reason = reason;
		sm_transition(SM_ERROR);
	}

	if (err_hdl.cb == NULL) {
		LOG_ERR("No fallback handler defined for state machine errors!");
		goto out;
	}

	ret = err_hdl.cb(err_reason, err_hdl.args);
	if (ret == 0) {
		sm_event("error mitigated, returning to IDLE");
		sm_backend_stop_timers();
		err_reason = SM_ERR_UNKNOWN;
		sm_transition(SM_IDLE);
	} else {
		LOG_ERR("State Machine error handler failed with code %d", ret);
	}

out:
	k_spin_unlock(&err_lock, key);
}

/* sm_error_retry – see state_internal.h */
void sm_error_retry(void)
{
	/* Try to fix the error; re-report the original cause. */
	sm_do_error_handling(err_reason);
}

#if defined(CONFIG_FILTER)
/* sm_filter_detect_apogee – see state_internal.h */
int sm_filter_detect_apogee(void)
{
	return filter_detect_apogee(&filter);
}
#endif /* CONFIG_FILTER */

/*-----------------------------------------------------------
 * Initialization / Deinitialization
 *----------------------------------------------------------*/

/* sm_init – see state.h */
void sm_init(const struct sm_thresholds *cfg,
	     struct sm_error_handling_args *sm_err_hdl)
{
#if defined(CONFIG_FILTER)
	int ret = filter_init(&filter);
	if (ret) {
		LOG_ERR("Could not initialize filter (%d).", ret);
		return;
	}
#endif /* CONFIG_FILTER */

	sm_backend_init(cfg);
	current_state = SM_IDLE;
	err_reason = SM_ERR_UNKNOWN;
	sm_event("state machine initialized");

	if (sm_err_hdl != NULL) {
		err_hdl.cb = sm_err_hdl->cb;
		err_hdl.args = sm_err_hdl->args;
	}
}

/* sm_deinit – see state.h */
void sm_deinit(void)
{
	sm_backend_deinit();
	memset(&last_inputs, 0, sizeof(last_inputs));
	sm_event("state machine reset");
	current_state = SM_IDLE;
	err_reason = SM_ERR_UNKNOWN;
}

/*-----------------------------------------------------------
 * Update
 *----------------------------------------------------------*/

/* sm_update – see state.h */
void sm_update(const struct sm_inputs *inputs)
{
	static double previous_altitude = 0.0;

#if defined(CONFIG_FILTER)
	static uint64_t last_time_ns = 0;
	struct sm_inputs filtered_inputs;

	uint64_t current_time_ns = k_ticks_to_ns_floor64(k_uptime_ticks());

	if (last_time_ns != 0) {
		filter_predict(&filter, current_time_ns - last_time_ns,
			       inputs->accel_vert);
		filter_update(&filter, inputs->altitude);
	}
	last_time_ns = current_time_ns;

	filtered_inputs = *inputs;
	filtered_inputs.altitude = filter.state[0];
	filtered_inputs.velocity = filter.state[1];

	sm_backend_step(&filtered_inputs, previous_altitude);
	previous_altitude = filtered_inputs.altitude;
	last_inputs = filtered_inputs;
#else
	sm_backend_step(inputs, previous_altitude);
	previous_altitude = inputs->altitude;
	last_inputs = *inputs;
#endif /* CONFIG_FILTER */
}

/* sm_update_force – see state.h */
void sm_update_force(enum sm_state transition_to)
{
	sm_transition(transition_to);
}

/*-----------------------------------------------------------
 * Getters
 *----------------------------------------------------------*/

/* sm_get_state – see state.h */
enum sm_state sm_get_state(void)
{
	return current_state;
}

/* sm_get_inputs – see state.h */
void sm_get_inputs(struct sm_inputs *out)
{
	*out = last_inputs;
}

/* sm_error_reason_str – see state.h */
const char *sm_error_reason_str(enum sm_error_reason reason)
{
	switch (reason) {
	case SM_ERR_UNKNOWN:		return "UNKNOWN";
	case SM_ERR_LOG_OFFLINE:	return "LOG_OFFLINE";
	case SM_ERR_APOGEE_TIMEOUT:	return "APOGEE_TIMEOUT";
	case SM_ERR_REDUNDANT_TIMEOUT:	return "REDUNDANT_TIMEOUT";
	default:			return "UNKNOWN";
	}
}
