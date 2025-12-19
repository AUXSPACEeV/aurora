/*
 * Copyright (c) 2025, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lib/state/simple.h>

LOG_MODULE_REGISTER(simple_state, CONFIG_STATE_MACHINE_LOG_LEVEL);

/*-----------------------------------------------------------
 * Internal State
 *----------------------------------------------------------*/
static enum sm_state current_state = SM_IDLE;
static struct sm_thresholds th;

static struct k_timer dt_ab;
static struct k_timer dt_l;

static struct k_timer to_a;
static struct k_timer to_m;
static struct k_timer to_r;

enum timers {
	TIMER_DT_AB = 0,
	TIMER_DT_L,
	NUM_TIMERS
};

static int running_timers[NUM_TIMERS] = {0};

/*-----------------------------------------------------------
 * Local Helpers
 *----------------------------------------------------------*/
#define TIMER_EXPIRED(tmr) (k_timer_status_get(tmr) > 0)

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

/*-----------------------------------------------------------
 * Initialization / Deinitialization
 *----------------------------------------------------------*/
void sm_init(const struct sm_thresholds *cfg)
{
	th = *cfg;
	init_timers();
	current_state = SM_IDLE;

	LOG_INF("State machine initialized (DISARMED, IDLE)");
}

void sm_deinit(void)
{
	th = (struct sm_thresholds){0};
	stop_timers();
	current_state = SM_IDLE;

	LOG_INF("State machine reset (DISARMED, IDLE)");
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
							  float previous_altitude)
{
	/* No matter the state, go to IDLE if disarmed */
	if (!in->armed) {
		stop_timers();
		current_state = SM_IDLE;
		LOG_INF("-[DISARM]-> IDLE");
	}

	switch (current_state)
	{

	/*-----------------------------------------------------------
	* IDLE -> ARMED
	*----------------------------------------------------------*/
	case SM_IDLE:
		if (in->armed && in->orientation >= th.T_OA) {
			current_state = SM_ARMED;
			LOG_INF("-[ARM]-> ARMED");
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
			current_state = SM_IDLE;
			LOG_INF("-[ORIENTATION]-> IDLE");
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
				/* Congrats! BOOST detected! */
				current_state = SM_BOOST;
				k_timer_stop(&dt_ab);
				running_timers[TIMER_DT_AB] = 0;
				LOG_INF("-> BOOST");
			}

			break;
		}

		/* Timer hasn't started yet. Are conditions met? */
		if (!arm_to_boost_conditions_met(in))
			break;

		/* Conditions are met, so start the timer. */
		k_timer_start(&dt_ab, K_MSEC(th.DT_AB), K_NO_WAIT);
		running_timers[TIMER_DT_AB] = 1;
		break;

	/*-----------------------------------------------------------
	* BOOST -> BURNOUT
	*----------------------------------------------------------*/
	case SM_BOOST:
		if (in->acceleration < th.T_BB) {
			current_state = SM_BURNOUT;
			LOG_INF("-> BURNOUT");
		}
		break;

	/*-----------------------------------------------------------
	* APOGEE detection - BURNOUT -> APOGEE
	*----------------------------------------------------------*/
	case SM_BURNOUT:
		if (in->velocity <= 0.0f && in->altitude < previous_altitude) {
			k_timer_start(&to_a, K_MSEC(th.TO_A), K_NO_WAIT);
			current_state = SM_APOGEE;
			LOG_INF("-> APOGEE");
		}
		break;

	/*-----------------------------------------------------------
	* APOGEE -> MAIN
	*----------------------------------------------------------*/
	case SM_APOGEE:
		if (in->altitude < th.T_M) {
			k_timer_stop(&to_a);
			current_state = SM_MAIN;
			LOG_INF("-> MAIN");
		} else if (TIMER_EXPIRED(&to_a)) {
			/* Timeout expired, abort to IDLE */
			k_timer_stop(&to_a);
			k_timer_start(&to_m, K_MSEC(th.TO_M), K_NO_WAIT);
			current_state = SM_IDLE;
			LOG_INF("-[TIMEOUT]-> IDLE");
		}
		break;

	/*-----------------------------------------------------------
	* MAIN -> REDUNDAND
	*----------------------------------------------------------*/
	case SM_MAIN:
		if (TIMER_EXPIRED(&to_m)) {
			k_timer_stop(&to_m);
			k_timer_start(&to_r, K_MSEC(th.TO_R), K_NO_WAIT);
			current_state = SM_REDUNDAND;
			LOG_INF("-> REDUNDAND");
		}
		break;

	/*-----------------------------------------------------------
	* REDUNDAND -> LANDED
	*----------------------------------------------------------*/
	case SM_REDUNDAND:
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
				current_state = SM_LANDED;
				k_timer_stop(&dt_l);
				running_timers[TIMER_DT_L] = 0;
				LOG_INF("-> LANDED");
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
			current_state = SM_IDLE;
			LOG_INF("-[TIMEOUT]-> IDLE");
		}
		break;

	/*-----------------------------------------------------------
	* Recovery -> Landed
	*----------------------------------------------------------*/
	case SM_LANDED:
		/* End state – do nothing */
		break;
	}
}

void sm_update(const struct sm_inputs *in)
{
	static float previous_altitude = 0.0f;

	_sm_update(in, previous_altitude);
	previous_altitude = in->altitude;
}

/*-----------------------------------------------------------
 * Getter
 *----------------------------------------------------------*/
enum sm_state sm_get_state(void)
{
	return current_state;
}
