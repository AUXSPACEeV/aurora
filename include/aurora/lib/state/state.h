/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_H_
#define APP_LIB_STATE_H_

/**
 * @defgroup lib_state State Machine library
 * @ingroup lib
 * @{
 *
 * @brief AURORA common state machine library for avionics.
 */

/*
 * The common API below is implemented once by the state core (state.c) and
 * dispatches to the selected backend through direct calls.  Its signatures
 * still reference backend-specific types (enum sm_state, struct sm_thresholds,
 * struct sm_inputs), so the matching backend *types* header is routed in here
 * by Kconfig.  Those headers live under internal/ and reject direct inclusion:
 * this is the single, backend-agnostic entry point applications include.
 * Each backend must define an sm_state enum that includes at least SM_IDLE and
 * SM_ERROR, which the common error path relies on.
 */
#define AURORA_STATE_BACKEND_INTERNAL
#if defined(CONFIG_SIMPLE_STATE)
#include <aurora/lib/state/internal/simple.h>
#else
#error "Unknown state machine type! Make sure CONFIG_AURORA_STATE_MACHINE_TYPE is set."
#endif /* CONFIG_SIMPLE_STATE */
#undef AURORA_STATE_BACKEND_INTERNAL

/*-----------------------------------------------------------
 * State machine type
 *----------------------------------------------------------*/

/**
 * @brief Identifier of the active state machine implementation.
 *
 * Each implementation defines its own @c sm_state enum, so any
 * external consumer (ground station, post-flight tooling) needs to
 * know which implementation produced a given state value before it
 * can decode it.
 */
enum sm_type {
    SM_TYPE_SIMPLE = 0,    /**< Simple backend (lib/state/simple.c). */
    /* SM_TYPE_TWO_STAGE = 1, ... */
};

/*-----------------------------------------------------------
 * Types
 *----------------------------------------------------------*/

/**
 * @brief Reasons the state machine can enter @c SM_ERROR.
 *
 * Passed to the error callback so the application can decide per cause
 * whether to recover (return 0 → back to IDLE) or hold the error state
 * (non-zero, e.g. a pre-flight interlock the operator must acknowledge
 * by disarming) and how to signal the operator (LED, buzzer, ...).
 */
enum sm_error_reason {
	SM_ERR_UNKNOWN = 0,       /**< Unspecified error. */
	SM_ERR_LOG_OFFLINE,       /**< Flight recorder unavailable while arming or armed. */
	SM_ERR_APOGEE_TIMEOUT,    /**< No descent detected within TO_A. */
	SM_ERR_REDUNDANT_TIMEOUT, /**< No landing detected within TO_R. */
};

/**
 * @brief Return a human-readable name for the given error reason.
 *
 * @param reason Error reason value.
 * @return Pointer to a static string, or "UNKNOWN" if invalid.
 */
const char *sm_error_reason_str(enum sm_error_reason reason);

/**
 * @brief Callback invoked when the state machine encounters an error.
 *
 * The implementation can define specific recovery logic.
 *
 * @param reason Why the state machine entered @c SM_ERROR.
 * @param args   Pointer to an implementation-specific config structure.
 *
 * @return 0 if the error was mitigated (state machine returns to IDLE),
 *         negative errno to hold @c SM_ERROR (the callback is re-invoked
 *         on every update until it mitigates or the system is disarmed).
 */
typedef int (*sm_error_cb_t) (enum sm_error_reason reason, void *args);

/**
 * @brief Error handling configuration for the state machine.
 */
struct sm_error_handling_args {
    sm_error_cb_t cb;   /**< Error callback function. */
    void *args;         /**< Opaque argument passed to @ref cb. */
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

/**
 * @brief Identify which state machine implementation is active.
 *
 * External consumers use it to pick the right @c sm_state enum mapping.
 *
 * @return Active state machine type ID.
 */
enum sm_type sm_get_type(void);

/**
 * @brief Retrieve the most recent inputs the state machine evaluated.
 *
 * When CONFIG_FILTER is enabled, ``altitude`` and ``velocity`` are the
 * Kalman-filtered values; the remaining fields are passed through from
 * the last sm_update() call.  When CONFIG_FILTER is disabled, all
 * fields are the raw sm_update() inputs (and ``velocity`` is whatever
 * the caller set, typically 0).
 *
 * Before the first sm_update() call the returned struct is zeroed.
 *
 * @param out Destination struct, must be non-NULL.
 */
void sm_get_inputs(struct sm_inputs *out);

/**
 * @brief Update the state machine with force. No further checks are done.
 *
 * @param transition_to State to transition.
 */
void sm_update_force(enum sm_state transition_to);

/** @} */

#endif /* APP_LIB_STATE_H_ */
