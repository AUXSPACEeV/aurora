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
static enum sm_state current_state = SM_DISARMED;
static struct sm_thresholds th;

static struct k_timer idle_timer;
static struct k_timer recovery_timer_1;
static struct k_timer recovery_timer_2;

/*-----------------------------------------------------------
 * Local Helpers
 *----------------------------------------------------------*/
#define TIMER_EXPIRED(tmr) (k_timer_status_get(tmr) > 0)

static void init_timers(void)
{
	k_timer_init(&idle_timer, NULL, NULL);
	k_timer_init(&recovery_timer_1, NULL, NULL);
	k_timer_init(&recovery_timer_2, NULL, NULL);
}

static void stop_timers(void)
{
	k_timer_stop(&idle_timer);
	k_timer_stop(&recovery_timer_2);
	k_timer_stop(&recovery_timer_1);
}

/*-----------------------------------------------------------
 * Initialization / Deinitialization
 *----------------------------------------------------------*/
void sm_init(const struct sm_thresholds *cfg)
{
	th = *cfg;
	init_timers();
	current_state = SM_DISARMED;

	LOG_INF("State machine initialized (DISARMED)");
}

void sm_deinit(void)
{
	th = (struct sm_thresholds){0};
	stop_timers();
	current_state = SM_DISARMED;

	LOG_INF("State machine reset (DISARMED)");
}


/*-----------------------------------------------------------
 * State Machine Update
 *----------------------------------------------------------*/
void sm_update(const struct sm_inputs *in)
{
	switch (current_state)
	{
	/*-----------------------------------------------------------
	* DISARMED -> IDLE
	*----------------------------------------------------------*/
	case SM_DISARMED:
		current_state = SM_IDLE;
		k_timer_start(&idle_timer, K_MSEC(th.T_L), K_NO_WAIT);
		LOG_INF("-> IDLE");
		break;

	/*-----------------------------------------------------------
	* IDLE -> DETECT LIFTOFF
	*----------------------------------------------------------*/
	case SM_IDLE:
		if (TIMER_EXPIRED(&idle_timer)) {
			current_state = SM_DETECT_LIFTOFF_AWAITING;
			LOG_INF("-> DETECT_LIFTOFF (AWAITING)");
		}
		break;

	/*-----------------------------------------------------------
	* DETECT_LIFTOFF: Awaiting -> Accelerating
	*----------------------------------------------------------*/
	case SM_DETECT_LIFTOFF_AWAITING:
		if (in->acceleration > th.T_A) {
			current_state = SM_DETECT_LIFTOFF_ACCELERATING;
			LOG_INF("-> DETECT_LIFTOFF (ACCELERATING)");
		}
		break;

	/*-----------------------------------------------------------
	* Accelerating -> Ascending OR back to awaiting
	*----------------------------------------------------------*/
	case SM_DETECT_LIFTOFF_ACCELERATING:
		if (in->acceleration < th.T_A) {
			current_state = SM_DETECT_LIFTOFF_AWAITING;
			LOG_INF("<- DETECT_LIFTOFF (back to AWAITING)");
		} else if (in->height > th.T_H) {
			current_state = SM_DETECT_LIFTOFF_ASCENDING;
			LOG_INF("-> DETECT_LIFTOFF (ASCENDING)");
		}
		break;

	/*-----------------------------------------------------------
	* Ascending -> Liftoff OR back to awaiting (falling)
	*----------------------------------------------------------*/
	case SM_DETECT_LIFTOFF_ASCENDING:
		if (in->height < th.T_H) {
			current_state = SM_DETECT_LIFTOFF_AWAITING;
			LOG_INF("<- DETECT_LIFTOFF (fallback to AWAITING)");
		} else {
			current_state = SM_LIFTOFF;
			LOG_INF("-> LIFTOFF");
		}
		break;

	/*-----------------------------------------------------------
	* Liftoff -> In-Flight
	*----------------------------------------------------------*/
	case SM_LIFTOFF:
		current_state = SM_IN_FLIGHT;
		LOG_INF("-> IN_FLIGHT");
		break;

	/*-----------------------------------------------------------
	* In-Flight -> Deploy recovery
	*----------------------------------------------------------*/
	case SM_IN_FLIGHT:
		if ((in->height <= th.T_Rd) || (in->height < in->previous_height)) {
			current_state = SM_DEPLOY_RECOVERY;
			k_timer_start(&recovery_timer_1, K_SECONDS(th.T_R), K_NO_WAIT);
			k_timer_start(&recovery_timer_2, K_SECONDS(th.T_R2), K_NO_WAIT);
			LOG_INF("-> DEPLOY_RECOVERY");
		}
		break;

	/*-----------------------------------------------------------
	* Recovery -> Landed
	*----------------------------------------------------------*/
	case SM_DEPLOY_RECOVERY:
		if ((in->height <= th.T_Lh) ||
			(in->acceleration <= th.T_La) ||
			TIMER_EXPIRED(&recovery_timer_1) ||
			TIMER_EXPIRED(&recovery_timer_2)) {

			current_state = SM_LANDED;
			LOG_INF("-> LANDED");
		}
		break;

	case SM_LANDED:
		/* End state – do nothing */
		break;
	}
}

/*-----------------------------------------------------------
 * Getter
 *----------------------------------------------------------*/
enum sm_state sm_get_state(void)
{
	return current_state;
}
