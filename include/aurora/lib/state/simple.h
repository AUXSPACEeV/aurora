/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_SIMPLE_H_
#define APP_LIB_STATE_SIMPLE_H_

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
	SM_IDLE = 0,	/**< System idle and unarmed, awaiting arm conditions. */
	SM_ARMED,		/**< Monitoring acceleration and altitude for liftoff start. */
	SM_BOOST,		/**< Acceleration confirmed. */
	SM_BURNOUT,		/**< Motor burnout. */
	SM_APOGEE,		/**< Apogee detected. */
	SM_MAIN,		/**< Main descent phase. */
	SM_REDUNDAND,	/**< Recovery devices are or should be deployed. */
	SM_LANDED,		/**< Rocket is confirmed landed. */
	SM_ERROR,		/**< State machine error occurred. */
};

/**
* @addtogroup lib_state
* @{
*/

/*-----------------------------------------------------------
 * Threshold definitions
 *----------------------------------------------------------*/

/**
 * @brief Threshold configuration for the rocket state machine.
 *
 * Defines thresholds for state transitions based on orientation,
 * altitude, acceleration, and timing.
 */
 struct sm_thresholds {
	/* Sensor Metrics */
	int T_AB;	/**< Acceleration threshold for ARMED -> BOOST transition. */
	int T_H;	/**< Altitude threshold for ARMED -> BOOST transition. */
	int T_BB;	/**< Acceleration threshold for BOOST -> BURNOUT transition. */
	int T_M;	/**< Descent height threshold for APOGEE -> MAIN transition. */
	int T_L;	/**< Velocity threshold for landing detection. */
	int T_OA;	/**< Orientation threshold for IDLE -> ARMED transition. */
	int T_OI;	/**< Orientation threshold for ARMED -> IDLE transition. */

	/* Timers */
	int DT_AB;	/**< Time for T_AB and T_H assertion (ms). */
	int DT_L;	/**< Time for T_L assertion (ms). */

	/* Timeouts */
	int TO_A;	/**< Max time allowed in APOGEE state before abort (ms). */
	int TO_M;	/**< Time between MAIN and REDUNDAND (ms). */
	int TO_R;	/**< Max time allowed in REDUNDAND state before abort (ms). */
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
	double orientation;		/**< Current orientation reading. */
	double acceleration;		/**< Current acceleration reading. */
	double velocity;			/**< Current vertical velocity. */
	double altitude;			/**< Current altitude measurement. */
};

/** @} */

#endif /* APP_LIB_STATE_SIMPLE_H_ */
