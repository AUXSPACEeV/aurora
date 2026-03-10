/*
 * Copyright (c) 2025 Auxspace e.V.
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
	SM_IDLE = 0,	/**< System unarmed armed, awaiting liftoff conditions. */
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
	int T_AB;	/**< Acceleration threshhold for ARMED -> BOOST transition */
	int T_H;	/**< Altitude thresshold for ARMED -> BOOST transition */
	int T_BB;	/**< Acceleration threshhold for BOOST -> BURNOUT transition */
	int T_M;	/**< Descent rate threshhold for APOGEE -> MAIN transition */
	int T_L;	/**< Velocity threshhold for Landing detection */
	int T_OA;	/**< Orientation threshhold for IDLE -> ARMED transition */
	int T_OI;	/**< Orientation threshhold for ARMED -> IDLE transition */

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

/** @} */

#endif /* APP_LIB_STATE_SIMPLE_H_ */
