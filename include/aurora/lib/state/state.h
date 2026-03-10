/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_H_
#define APP_LIB_STATE_H_

#if defined(CONFIG_SIMPLE_STATE)
#include <aurora/lib/state/simple.h>
#else
#error "Unknown state machine type! Make sure CONFIG_STATE_MACHINE_TYPE is set."
#endif /* CONFIG_SIMPLE_STATE */

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
 * Types
 *----------------------------------------------------------*/

/**
 * @brief Callback when the state machine encounters an error.
 *
 * This function is called when the state machine encounters an error and
 * tries to recover. The implementation can define specific recovery.
 *
 * @param args Pointer to an implementation specific config structure.
 * 
 * @return Status code (0 = error could be mitigated, other = error code)
 */
typedef int (*sm_error_cb_t) (void *args);

struct sm_error_handling_args {
    sm_error_cb_t cb;
    void *args;
};

/*-----------------------------------------------------------
 * API
 *----------------------------------------------------------*/

/**
 * @brief Initialize the rocket state machine.
 *
 * This function prepares the state machine, loads the threshold
 * configuration, initializes internal timers, and sets the
 * initial state to `SM_IDLE`.
 *
 * @param cfg Pointer to a threshold configuration structure.
 * @param err_hdl Pointer to an error handling configuration (callback + args), or NULL.
 */
void sm_init(const struct sm_thresholds *cfg,
             struct sm_error_handling_args *err_hdl);

/**
 * @brief Deinitialize the rocket state machine.
 *
 * This function resets the state machine, unloads the threshold
 * configuration, stops internal timers, and sets the
 * initial state.
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
 * @return Current state
 * (usually an enum implementation in state implementation).
 */
enum sm_state sm_get_state(void);

/** @} */

#endif /* APP_LIB_STATE_H_ */
