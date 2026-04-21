/**
 * @file simple_state.c
 * @brief Simple 9-state flight state machine implementation.
 *
 * Implements the flight state sequence:
 * IDLE -> ARMED -> BOOST -> BURNOUT -> APOGEE -> MAIN -> REDUNDANT -> LANDED / ERROR
 *
 * State transitions are driven by sensor thresholds and timers.
 * Optionally integrates with the Kalman filter input filtering.
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <aurora/lib/state/state.h>

#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)
#include <aurora/lib/state/audit.h>
#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */

#if defined(CONFIG_FILTER)
#include <aurora/lib/filter.h>
static struct filter filter;
#endif /* CONFIG_FILTER */

LOG_MODULE_REGISTER(simple_state, CONFIG_STATE_MACHINE_LOG_LEVEL);

/*-----------------------------------------------------------
 * Prototypes
 *----------------------------------------------------------*/

/** @brief Initialize all internal Zephyr timers used by the state machine. */
static void init_timers(void);

/** @brief Stop all running timers and clear the @ref running_timers flags. */
static void stop_timers(void);

/**
 * @brief Default error handler when no user callback is registered.
 *
 * @param args Unused opaque argument (always NULL).
 * @return Always returns -EIO.
 */
static int fallback_sm_error_handler(void *args);

/**
 * @brief Transition to SM_ERROR and invoke the error callback.
 *
 * Acquires @ref err_lock, sets state to SM_ERROR, and calls the
 * registered error callback (or the fallback).
 */
static void sm_do_error_handling(void);

/**
 * @brief Check if acceleration and altitude meet ARMED->BOOST thresholds.
 *
 * @param in Pointer to the current sensor input values.
 * @retval true  Both acceleration >= T_AB and altitude >= T_H.
 * @retval false One or both thresholds are not met.
 */
static inline bool arm_to_boost_conditions_met(const struct sm_inputs *in);

/**
 * @brief Core state machine update logic.
 *
 * Evaluates sensor inputs against thresholds and timers to determine
 * state transitions.  Called by sm_update() with additional bookkeeping.
 *
 * @param in                Pointer to the current sensor input values.
 * @param previous_altitude Altitude from the previous update cycle (m).
 */
static inline void _sm_update(const struct sm_inputs *in,
			      double previous_altitude);

/*-----------------------------------------------------------
 * Internal State
 *----------------------------------------------------------*/
static enum sm_state current_state = SM_IDLE; /**< Active flight state. */
static struct sm_thresholds th; /**< Loaded threshold configuration. */

static struct k_spinlock err_lock; /**< Spinlock protecting error callback invocation. */
static struct sm_error_handling_args err_hdl = {
	.cb = &fallback_sm_error_handler,
	.args = NULL,
};

static struct k_timer dt_ab; /**< Duration timer for ARMED->BOOST assertion. */
static struct k_timer dt_l;  /**< Duration timer for landing velocity assertion. */

static struct k_timer to_a; /**< Timeout timer for APOGEE state. */
static struct k_timer to_m; /**< Timeout timer for MAIN state. */
static struct k_timer to_r; /**< Timeout timer for REDUNDANT state. */

/** @brief Indices into the @ref running_timers array. */
enum timers {
	TIMER_DT_AB = 0, /**< Index for the ARMED->BOOST duration timer. */
	TIMER_DT_L,      /**< Index for the landing duration timer. */
	NUM_TIMERS        /**< Number of tracked timers. */
};

static int running_timers[NUM_TIMERS] = {0}; /**< Per-timer running flag. */

/*-----------------------------------------------------------
 * Local Helpers
 *----------------------------------------------------------*/

/**
 * @brief Check whether a Zephyr timer has expired.
 *
 * @param tmr Pointer to a @c k_timer instance.
 * @return Non-zero if the timer has expired.
 */
#define TIMER_EXPIRED(tmr) (k_timer_status_get(tmr) > 0)

#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)
#define SM_TRANSITION(new_state) do {				\
	sm_audit_transition(current_state, (new_state));	\
	current_state = (new_state);				\
} while (0)
#define SM_EVENT(msg) sm_audit_event(current_state, (msg))
#else
#define SM_TRANSITION(new_state) do { current_state = (new_state); } while (0)
#define SM_EVENT(msg)
#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */

static void init_timers(void)
{
	k_timer_init(&dt_ab, NULL, NULL);
	k_timer_init(&dt_l, NULL, NULL);

	k_timer_init(&to_a, NULL, NULL);
	k_timer_init(&to_m, NULL, NULL);
	k_timer_init(&to_r, NULL, NULL);

	memset(running_timers, 0, sizeof(running_timers));
}

static void stop_timers(void)
{
	k_timer_stop(&dt_ab);
	k_timer_stop(&dt_l);

	k_timer_stop(&to_a);
	k_timer_stop(&to_m);
	k_timer_stop(&to_r);

	memset(running_timers, 0, sizeof(running_timers));
}

static int fallback_sm_error_handler(void *args)
{
	LOG_ERR("State Machine encountered an unrecoverable error.");

	return -EIO;
}

static void sm_do_error_handling(void)
{
	int ret;
	k_spinlock_key_t key = k_spin_lock(&err_lock);

	if (current_state != SM_ERROR) {
		SM_TRANSITION(SM_ERROR);
	}

	if (err_hdl.cb == NULL) {
		LOG_ERR("No fallback handler defined for state machine errors!");
		goto out;
	}

	ret = err_hdl.cb(err_hdl.args);
	if (ret == 0) {
		SM_EVENT("error mitigated, returning to IDLE");
		stop_timers();
		SM_TRANSITION(SM_IDLE);
	} else {
		LOG_ERR("State Machine error handler failed with code %d", ret);
	}

out:
	k_spin_unlock(&err_lock, key);
}

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

	th = *(struct sm_thresholds *)cfg;
	init_timers();
	current_state = SM_IDLE;
	SM_EVENT("state machine initialized");

	if (sm_err_hdl != NULL) {
		err_hdl.cb = sm_err_hdl->cb;
		err_hdl.args = sm_err_hdl->args;
	}
}

/* sm_deinit – see state.h */
void sm_deinit(void)
{
	memset(&th, 0, sizeof(th));
	stop_timers();
	SM_EVENT("state machine reset");
	current_state = SM_IDLE;
}

/*-----------------------------------------------------------
 * Helper for ARM -> BOOST conditions
 *----------------------------------------------------------*/
static inline bool arm_to_boost_conditions_met(const struct sm_inputs *in)
{
	return (in->acceleration >= th.T_AB &&
			in->altitude >= th.T_H);
}

/*-----------------------------------------------------------
 * State Machine Update
 *----------------------------------------------------------*/
static inline void _sm_update(const struct sm_inputs *in,
			      double previous_altitude)
{
	/* go to IDLE if disarmed */
	if (!in->armed && current_state != SM_IDLE) {
		stop_timers();
		SM_TRANSITION(SM_IDLE);
	}

	switch (current_state)
	{

	/*-----------------------------------------------------------
	* IDLE -> ARMED
	*----------------------------------------------------------*/
	case SM_IDLE:
		if (in->armed && in->orientation >= th.T_OA) {
			SM_TRANSITION(SM_ARMED);
		}
		break;

	/*-----------------------------------------------------------
	* ARMED -> BOOST
	*----------------------------------------------------------*/
	case SM_ARMED:
		if (in->orientation < th.T_OI) {
			/* Go back to IDLE if orientation is bad */
			running_timers[TIMER_DT_AB] = 0;
			k_timer_stop(&dt_ab);
			SM_EVENT("orientation below threshold");
			SM_TRANSITION(SM_IDLE);
			break;
		}

		/* Timer is running_timers running. Check conditions */
		if (running_timers[TIMER_DT_AB] == 1) {
			/* conditions aren't met. Reset the timer */
			if (!arm_to_boost_conditions_met(in)) {
				running_timers[TIMER_DT_AB] = 0;
				k_timer_stop(&dt_ab);
				LOG_INF("BOOST conditions not met, timer reset");
				break;
			}

			/* At this point, conditions are met. Is the timer done as well? */
			if (TIMER_EXPIRED(&dt_ab)) {
				SM_EVENT("orientation, altitude and timing threshold reached");
				/* Congrats! BOOST detected! */
				SM_TRANSITION(SM_BOOST);
				k_timer_stop(&dt_ab);
				running_timers[TIMER_DT_AB] = 0;
			}

			break;
		}

		/* Timer hasn't started yet. Are conditions met? */
		if (!arm_to_boost_conditions_met(in))
			break;

		SM_EVENT("orientation and altitude threshold reached");
		/* Conditions are met, so start the timer. */
		k_timer_start(&dt_ab, K_MSEC(th.DT_AB), K_NO_WAIT);
		running_timers[TIMER_DT_AB] = 1;
		break;

	/*-----------------------------------------------------------
	* BOOST -> BURNOUT
	*----------------------------------------------------------*/
	case SM_BOOST:
		if (in->acceleration < th.T_BB) {
			SM_TRANSITION(SM_BURNOUT);
		}
		break;

	/*-----------------------------------------------------------
	* APOGEE detection - BURNOUT -> APOGEE
	*----------------------------------------------------------*/
	case SM_BURNOUT:
#if defined(CONFIG_FILTER)
		if (filter_detect_apogee(&filter) == 1) {
			k_timer_start(&to_a, K_MSEC(th.TO_A), K_NO_WAIT);
			SM_TRANSITION(SM_APOGEE);
		}
#else
		if (in->velocity <= 0.0 && in->altitude < previous_altitude) {
			k_timer_start(&to_a, K_MSEC(th.TO_A), K_NO_WAIT);
			SM_TRANSITION(SM_APOGEE);
		}
#endif /* CONFIG_FILTER */
		break;

	/*-----------------------------------------------------------
	* APOGEE -> MAIN
	*----------------------------------------------------------*/
	case SM_APOGEE:
		if (in->altitude < th.T_M) {
			k_timer_stop(&to_a);
			k_timer_start(&to_m, K_MSEC(th.TO_M), K_NO_WAIT);
			SM_TRANSITION(SM_MAIN);
		} else if (TIMER_EXPIRED(&to_a)) {
			/* Timeout expired, abort to ERROR */
			k_timer_stop(&to_a);
			SM_EVENT("apogee timeout expired");
			sm_do_error_handling();
		}
		break;

	/*-----------------------------------------------------------
	* MAIN -> REDUNDANT
	*----------------------------------------------------------*/
	case SM_MAIN:
		if (TIMER_EXPIRED(&to_m)) {
			k_timer_stop(&to_m);
			k_timer_start(&to_r, K_MSEC(th.TO_R), K_NO_WAIT);
			SM_TRANSITION(SM_REDUNDANT);
		}
		break;

	/*-----------------------------------------------------------
	* REDUNDANT -> LANDED
	*----------------------------------------------------------*/
	case SM_REDUNDANT:
		/* Timer is running. Check conditions */
		if (running_timers[TIMER_DT_L] == 1) {
			/* conditions aren't met. Reset the timer */
			if (in->velocity > th.T_L) {
				running_timers[TIMER_DT_L] = 0;
				k_timer_stop(&dt_l);
				goto _check_timeout;
			}

			/* At this point, conditions are met. Is the timer done as well? */
			if (TIMER_EXPIRED(&dt_l)) {
				/* Congrats! LANDING detected! */
				SM_TRANSITION(SM_LANDED);
				k_timer_stop(&dt_l);
				running_timers[TIMER_DT_L] = 0;
				break;
			}

			goto _check_timeout;
		}

		/* Timer hasn't started yet. Are conditions met? */
		if (in->velocity > th.T_L)
			goto _check_timeout;

		/* Conditions are met, so start the timer. */
		k_timer_start(&dt_l, K_MSEC(th.DT_L), K_NO_WAIT);
		running_timers[TIMER_DT_L] = 1;

_check_timeout:
		/* Quick check the timeout */
		if (TIMER_EXPIRED(&to_r)) {
			/* Timeout expired, abort to IDLE */
			k_timer_stop(&dt_l);
			running_timers[TIMER_DT_L] = 0;
			k_timer_stop(&to_r);
			SM_EVENT("redundant timeout expired");
			sm_do_error_handling();
		}
		break;
	/*-----------------------------------------------------------
	* ERROR state
	*----------------------------------------------------------*/
	case SM_ERROR:
		/* Try to fix the error */
		sm_do_error_handling();
		break;

	/*-----------------------------------------------------------
	* Recovery -> Landed
	*----------------------------------------------------------*/
	case SM_LANDED:
		/* End state – do nothing */
		break;
	}
}

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

	_sm_update(&filtered_inputs, previous_altitude);
	previous_altitude = filtered_inputs.altitude;
#else
	_sm_update(inputs, previous_altitude);
	previous_altitude = inputs->altitude;
#endif /* CONFIG_FILTER */
}

/*-----------------------------------------------------------
 * Getter
 *----------------------------------------------------------*/

/* sm_get_state – see state.h */
enum sm_state sm_get_state(void)
{
	return current_state;
}

/* sm_state_str – see simple.h */
const char *sm_state_str(enum sm_state state)
{
	switch (state) {
	case SM_IDLE:		return "IDLE";
	case SM_ARMED:		return "ARMED";
	case SM_BOOST:		return "BOOST";
	case SM_BURNOUT:	return "BURNOUT";
	case SM_APOGEE:		return "APOGEE";
	case SM_MAIN:		return "MAIN";
	case SM_REDUNDANT:	return "REDUNDANT";
	case SM_LANDED:		return "LANDED";
	case SM_ERROR:		return "ERROR";
	default:		return "UNKNOWN";
	}
}
