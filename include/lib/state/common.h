/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_COMMON_H_
#define APP_LIB_STATE_COMMON_H_

/**
* @defgroup lib_state Common State Machine library
* @ingroup lib
* @{
*
* @brief AURORA Common State Machine library for avionics.
*
* This library contains common State Machine functions.
*/

/*-----------------------------------------------------------
 * States
 *----------------------------------------------------------*/

/**
 * @brief State identifiers for the rocket state machine.
 *
 * These represent the discrete system states from power-on through
 * flight, recovery, and landing detection.
 */
enum sm_state {
	SM_DISARMED = 0,				/**< System is powered but not armed. */
	SM_IDLE,						/**< System armed, awaiting liftoff conditions. */
	SM_DETECT_LIFTOFF_AWAITING,		/**< Monitoring acceleration for liftoff start. */
	SM_DETECT_LIFTOFF_ACCELERATING,	/**< Acceleration exceeds threshold, confirming thrust. */
	SM_DETECT_LIFTOFF_ASCENDING,	/**< Height confirmation of ascent. */
	SM_LIFTOFF,						/**< Liftoff event confirmed. */
	SM_IN_FLIGHT,					/**< Main flight phase until apogee/peak descent. */
	SM_DEPLOY_RECOVERY,				/**< Recovery devices are or should be deployed. */
	SM_LANDED,						/**< Rocket is confirmed landed. */
};

/** @} */

#endif /* APP_LIB_STATE_COMMON_H_ */
