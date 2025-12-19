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
	SM_IDLE = 0,	/**< System unarmed armed, awaiting liftoff conditions. */
	SM_ARMED,		/**< Monitoring acceleration and altitude for liftoff start. */
	SM_BOOST,		/**< Acceleration confirmed. */
	SM_BURNOUT,		/**< Motor burnout. */
	SM_APOGEE,		/**< Apogee detected. */
	SM_MAIN,		/**< Main descent phase. */
	SM_REDUNDAND,	/**< Recovery devices are or should be deployed. */
	SM_LANDED,		/**< Rocket is confirmed landed. */
};

/** @} */

#endif /* APP_LIB_STATE_COMMON_H_ */
