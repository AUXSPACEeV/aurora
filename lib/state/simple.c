/**
 * @file simple.c
 * @brief Simple 9-state flight state machine backend.
 *
 * Implements the flight state sequence:
 * IDLE -> ARMED -> BOOST -> BURNOUT -> APOGEE -> MAIN -> REDUNDANT -> LANDED / ERROR
 *
 * State transitions are driven by sensor thresholds and timers.
 * Optionally integrates with the Kalman filter input filtering.
 *
 * Only the implementation-specific pieces live here: the threshold set,
 * the per-state transition logic and its timers.  The common lifecycle,
 * update entry point and error handling are provided by the state core
 * (state.c); this backend drives it through the helpers in
 * state_internal.h.
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/state/state.h>
#include "state_internal.h"

#ifndef M_PI
#define M_PI ((double)3.1415926535)
#endif

LOG_MODULE_DECLARE(state_machine, CONFIG_STATE_MACHINE_LOG_LEVEL);

/*-----------------------------------------------------------
 * Prototypes
 *----------------------------------------------------------*/

/** @brief Initialize all internal Zephyr timers used by the state machine. */
static void init_timers(void);

/**
 * @brief Check if acceleration and altitude meet ARMED->BOOST thresholds.
 *
 * @param in Pointer to the current sensor input values.
 * @retval true  Both acceleration >= T_AB and altitude >= T_H.
 * @retval false One or both thresholds are not met.
 */
static inline bool arm_to_boost_conditions_met(const struct sm_inputs *in);

/*-----------------------------------------------------------
 * Internal State
 *----------------------------------------------------------*/
static struct sm_thresholds th; /**< Loaded threshold configuration. */

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

/**
 * @brief Elevation of the configured up axis from horizontal (degrees).
 *
 * The orientation vector produced by imu_sensor_value_to_orientation()
 * already accounts for @c CONFIG_IMU_UP_AXIS_* by remapping the body
 * "up" axis to local Z, so pitch and yaw alone are sufficient to
 * recover the up-axis tilt:
 *   gz/|g| = cos(pitch) * cos(yaw)
 *
 * @return Elevation in degrees, clamped to [-90, 90].  +90 = up axis
 *         points to the sky, 0 = horizontal, -90 = inverted.
 */
static inline double sm_orientation_elevation_deg(const double orientation[3])
{
	const double deg2rad = M_PI / 180.0;
	double s = cos(orientation[1] * deg2rad) * cos(orientation[0] * deg2rad);
	if (s > 1.0)  s = 1.0;
	if (s < -1.0) s = -1.0;
	return asin(s) * (180.0 / M_PI);
}

static void init_timers(void)
{
	k_timer_init(&dt_ab, NULL, NULL);
	k_timer_init(&dt_l, NULL, NULL);

	k_timer_init(&to_a, NULL, NULL);
	k_timer_init(&to_m, NULL, NULL);
	k_timer_init(&to_r, NULL, NULL);

	memset(running_timers, 0, sizeof(running_timers));
}

/* sm_backend_stop_timers – see state_internal.h */
void sm_backend_stop_timers(void)
{
	k_timer_stop(&dt_ab);
	k_timer_stop(&dt_l);

	k_timer_stop(&to_a);
	k_timer_stop(&to_m);
	k_timer_stop(&to_r);

	memset(running_timers, 0, sizeof(running_timers));
}

static inline bool arm_to_boost_conditions_met(const struct sm_inputs *in)
{
	return (in->acceleration >= th.T_AB &&
		in->altitude >= th.T_H);
}

/*-----------------------------------------------------------
 * Initialization / Deinitialization (see state_internal.h)
 *----------------------------------------------------------*/

/* sm_backend_init – see state_internal.h */
void sm_backend_init(const struct sm_thresholds *cfg)
{
	th = *cfg;
	init_timers();
}

/* sm_backend_deinit – see state_internal.h */
void sm_backend_deinit(void)
{
	memset(&th, 0, sizeof(th));
	sm_backend_stop_timers();
}

/*-----------------------------------------------------------
 * State Machine Update (see state_internal.h)
 *----------------------------------------------------------*/

/* sm_backend_step – see state_internal.h */
void sm_backend_step(const struct sm_inputs *in, double previous_altitude)
{
	static int n_oi;

	/* go to IDLE if disarmed */
	if (!in->armed && sm_get_state() != SM_IDLE) {
		sm_backend_stop_timers();
		sm_transition(SM_IDLE);
	}

	switch (sm_get_state())
	{

	/*-----------------------------------------------------------
	* IDLE -> ARMED
	*----------------------------------------------------------*/
	case SM_IDLE:
		n_oi = 0;
		if (in->armed && sm_orientation_elevation_deg(in->orientation) >= th.T_OA) {
			if (in->log_ready) {
				sm_transition(SM_ARMED);
			} else {
				/* Arm conditions are met but the flight log is
				 * offline.  Refuse to arm so the vehicle never
				 * flies (or fires pyros) without recording, and
				 * surface it through the error path so the
				 * operator gets an unmissable field indication
				 * (LED/buzzer via the app error callback).
				 */
				sm_event("arm refused: flight log offline");
				sm_do_error_handling(SM_ERR_LOG_OFFLINE);
			}
		}
		break;

	/*-----------------------------------------------------------
	* ARMED -> BOOST
	*----------------------------------------------------------*/
	case SM_ARMED:
		/* The flight log went away after arming (e.g. the recorder
		 * failed to open on the IDLE->ARMED transition).  Still on
		 * the pad, so abort through the error path rather than fly
		 * unrecorded; the app error callback signals the operator
		 * and decides whether to hold SM_ERROR or recover to IDLE.
		 * Deliberately checked only here: from BOOST onward a log
		 * dropout must never abort the flight or recovery logic.
		 */
		if (!in->log_ready) {
			running_timers[TIMER_DT_AB] = 0;
			k_timer_stop(&dt_ab);
			sm_event("arm aborted: flight log offline");
			sm_do_error_handling(SM_ERR_LOG_OFFLINE);
			break;
		}

		if (sm_orientation_elevation_deg(in->orientation) < th.T_OI) {
			if (++n_oi < th.N_OI)
				break;

			/* Go back to IDLE if orientation is bad */
			running_timers[TIMER_DT_AB] = 0;
			k_timer_stop(&dt_ab);
			sm_event("orientation below threshold");
			sm_transition(SM_IDLE);
			break;
		}
		n_oi = 0;

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
				n_oi = 0;
				sm_event("orientation, altitude and timing threshold reached");
				/* Congrats! BOOST detected! */
				sm_transition(SM_BOOST);
				k_timer_stop(&dt_ab);
				running_timers[TIMER_DT_AB] = 0;
			}

			break;
		}

		/* Timer hasn't started yet. Are conditions met? */
		if (!arm_to_boost_conditions_met(in))
			break;

		sm_event("orientation and altitude threshold reached");
		/* Conditions are met, so start the timer. */
		k_timer_start(&dt_ab, K_MSEC(th.DT_AB), K_NO_WAIT);
		running_timers[TIMER_DT_AB] = 1;
		break;

	/*-----------------------------------------------------------
	* BOOST -> BURNOUT
	*----------------------------------------------------------*/
	case SM_BOOST:
		if (in->acceleration < th.T_BB) {
			sm_transition(SM_BURNOUT);
		}
		break;

	/*-----------------------------------------------------------
	* APOGEE detection - BURNOUT -> APOGEE
	*----------------------------------------------------------*/
	case SM_BURNOUT:
#if defined(CONFIG_FILTER)
		if (sm_filter_detect_apogee() == 1) {
			k_timer_start(&to_a, K_MSEC(th.TO_A), K_NO_WAIT);
			sm_transition(SM_APOGEE);
		}
#else
		if (in->velocity <= 0.0 && in->altitude < previous_altitude) {
			k_timer_start(&to_a, K_MSEC(th.TO_A), K_NO_WAIT);
			sm_transition(SM_APOGEE);
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
			sm_transition(SM_MAIN);
		} else if (TIMER_EXPIRED(&to_a)) {
			/* Timeout expired, abort to ERROR */
			k_timer_stop(&to_a);
			sm_event("apogee timeout expired");
			sm_do_error_handling(SM_ERR_APOGEE_TIMEOUT);
		}
		break;

	/*-----------------------------------------------------------
	* MAIN -> REDUNDANT
	*----------------------------------------------------------*/
	case SM_MAIN:
		if (TIMER_EXPIRED(&to_m)) {
			k_timer_stop(&to_m);
			k_timer_start(&to_r, K_MSEC(th.TO_R), K_NO_WAIT);
			sm_transition(SM_REDUNDANT);
		}
		break;

	/*-----------------------------------------------------------
	* REDUNDANT -> LANDED
	*----------------------------------------------------------*/
	case SM_REDUNDANT:
		/* Timer is running. Check conditions */
		if (running_timers[TIMER_DT_L] == 1) {
			/* conditions aren't met. Reset the timer */
			if (fabs(in->velocity) > (double)th.T_L) {
				running_timers[TIMER_DT_L] = 0;
				k_timer_stop(&dt_l);
				goto _check_timeout;
			}

			/* At this point, conditions are met. Is the timer done as well? */
			if (TIMER_EXPIRED(&dt_l)) {
				/* Congrats! LANDING detected! */
				sm_transition(SM_LANDED);
				k_timer_stop(&dt_l);
				running_timers[TIMER_DT_L] = 0;
				break;
			}

			goto _check_timeout;
		}

		/* Timer hasn't started yet. Are conditions met? */
		if (fabs(in->velocity) > (double)th.T_L)
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
			sm_event("redundant timeout expired");
			sm_do_error_handling(SM_ERR_REDUNDANT_TIMEOUT);
		}
		break;
	/*-----------------------------------------------------------
	* ERROR state
	*----------------------------------------------------------*/
	case SM_ERROR:
		/* Try to fix the error; re-report the original cause. */
		sm_error_retry();
		break;

	/*-----------------------------------------------------------
	* Recovery -> Landed
	*----------------------------------------------------------*/
	case SM_LANDED:
		/* End state – do nothing */
		break;
	}
}

/*-----------------------------------------------------------
 * Getters
 *----------------------------------------------------------*/

/* sm_get_type - see state.h */
enum sm_type sm_get_type(void)
{
	return SM_TYPE_SIMPLE;
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
