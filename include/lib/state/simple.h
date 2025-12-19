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
	/* Sensor Metrics */
	float T_AB;	/**< Acceleration threshhold for ARMED -> BOOST transition */
	float T_H;	/**< Altitude thresshold for ARMED -> BOOST transition */
	float T_BB;	/**< Acceleration threshhold for BOOST -> BURNOUT transition */
	float T_M;	/**< Descent rate threshhold for APOGEE -> MAIN transition */
	float T_L;	/**< Velocity threshhold for Landing detection */
	float T_OA;	/**< Orientation threshhold for IDLE -> ARMED transition */
	float T_OI;	/**< Orientation threshhold for ARMED -> IDLE transition */

	/* Timers */
	int DT_AB;	/**< Time for T_AB an T_H assertion (ms) */
	int DT_L;	/**< Time for T_L assertion (ms) */

	/* Timeouts */
	int TO_A;	/**< Max time allowed in APOGEE state before abort (ms) */
	int TO_M;	/**< Time between MAIN and REDUNDAND (ms) */
	int TO_R;	/**< Max time allowed in REDUNDAND state before abort (ms) */
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
	int armed;				/**< System armed status (non-zero = armed). */
	float orientation;		/**< Current orientation reading. */
	float acceleration;		/**< Current acceleration reading. */
	float velocity;			/**< Current vertical velocity. */
	float altitude;			/**< Current altitude measurement. */
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
