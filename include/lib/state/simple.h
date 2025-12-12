/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_SIMPLE_H_
#define APP_LIB_STATE_SIMPLE_H_

#include "common.h"

/**
* @defgroup lib_state Simple State Machine library
* @ingroup lib
* @{
*
* @brief AURORA Simple State Machine library for avionics.
*
* This library contains State Machine functions for the Simple State Machine.
*/

/*-----------------------------------------------------------
 * Threshold definitions – replace with real values
 *----------------------------------------------------------*/

/**
 * @brief Threshold configuration for the rocket state machine.
 *
 * These values define the various thresholds used for determining
 * transitions between states (battery level, orientation, altitude,
 * acceleration, and timing).
 */
 struct sm_thresholds {
	float	T_D;	/**< Required orientation threshold to allow liftoff detection. */
	float	T_A;	/**< Acceleration threshold for detecting active thrust. */
	float	T_H;	/**< Height threshold to confirm ascent. */
	float	T_Rd;	/**< Height threshold for initiating recovery device deployment. */
	float	T_Lh;	/**< Height threshold to detect landing. */
	float	T_La;	/**< Acceleration threshold to detect landing shock. */
	int		T_L;	/**< Time spent in Idle state before checking liftoff (ms). */
	int		T_R;	/**< Timeout before triggering recovery logic (first timer, ms). */
	int		T_R2;	/**< Timeout before triggering secondary recovery logic (ms). */
};

/*-----------------------------------------------------------
 * Inputs (sensor readings)
 *----------------------------------------------------------*/


/**
 * @brief Sensor input structure for the state machine.
 *
 * These values must be filled each update cycle to evaluate
 * state transitions.
 */
struct sm_inputs {
	float orientation;		/**< Current orientation reading. */
	float acceleration;		/**< Current acceleration reading. */
	float height;			/**< Current altitude measurement. */
	float previous_height;	/**< Previous altitude value (for descent detection). */
};

/*-----------------------------------------------------------
 * API
 *----------------------------------------------------------*/

/**
 * @brief Initialize the rocket state machine.
 *
 * This function prepares the state machine, loads the threshold
 * configuration, initializes internal timers, and sets the
 * initial state to @ref SM_DISARMED.
 *
 * @param cfg Pointer to a threshold configuration structure.
 */
void sm_init(const struct sm_thresholds *cfg);

/**
 * @brief Deinitialize the rocket state machine.
 *
 * This function resets the state machine, unloads the threshold
 * configuration, stops internal timers, and sets the
 * initial state to @ref SM_DISARMED.
 */
void sm_deinit(void);

/**
 * @brief Update the state machine using current sensor readings.
 *
 * Function evaluates sensor data and executes state transitions
 * according to the flight logic diagram. Must be called regularly
 * (e.g. at sensor update rate).
 *
 * @param inputs Pointer to populated sensor readings.
 */
void sm_update(const struct sm_inputs *inputs);

/**
 * @brief Retrieve the current state of the state machine.
 *
 * @return Current state (see @ref enum sm_state).
 */
enum sm_state sm_get_state(void);

/** @} */

#endif /* APP_LIB_STATE_SIMPLE_H_ */
