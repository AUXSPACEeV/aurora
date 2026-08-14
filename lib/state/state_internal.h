/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_INTERNAL_H_
#define APP_LIB_STATE_INTERNAL_H_

#include <stdbool.h>

#include <aurora/lib/state/state.h>

/**
 * @file state_internal.h
 * @brief Private contract between the common state core and a backend.
 *
 * The common core (state.c) implements the public API from state.h,
 * owns the shared runtime state (current state, error handling, input
 * filtering) and calls into the selected backend for the parts that
 * genuinely differ per implementation (the transition logic and its
 * private resources).  The backend, in turn, calls back into the core
 * through the helpers below to change state, log events and raise errors.
 *
 * This header is internal to the state library: it is deliberately not
 * installed under include/ and must not be used by application code.
 *
 * Backends are required to define an @c sm_state enum that includes at
 * least @c SM_IDLE and @c SM_ERROR, since the common error path uses them.
 */

/*-----------------------------------------------------------
 * Common core -> backend (defined by state.c)
 *----------------------------------------------------------*/

/**
 * @brief Transition to @p new_state, recording it in the audit log first.
 *
 * This is the only sanctioned way to change the active state; it keeps
 * the audit log and @ref sm_get_state() consistent.
 *
 * @param new_state State to transition to.
 */
void sm_transition(enum sm_state new_state);

/**
 * @brief Record a notable event against the current state in the audit log.
 *
 * @param msg Short event description (string literal or static storage).
 */
void sm_event(const char *msg);

/**
 * @brief Enter @c SM_ERROR for @p reason and run the error callback.
 *
 * Latches @p reason, transitions to @c SM_ERROR (if not already there)
 * and invokes the registered error callback.  If the callback clears the
 * error (returns 0) the core stops the backend timers and returns to
 * @c SM_IDLE.  Safe to call repeatedly from the @c SM_ERROR state.
 *
 * @param reason Why the state machine is entering @c SM_ERROR.
 */
void sm_do_error_handling(enum sm_error_reason reason);

/**
 * @brief Re-run the error handler for the currently latched reason.
 *
 * Used by the backend's @c SM_ERROR case to keep retrying recovery while
 * reporting the original cause.
 */
void sm_error_retry(void);

#if defined(CONFIG_FILTER)
/**
 * @brief Query the input filter for apogee (descent onset).
 *
 * @retval 1 Apogee detected.
 * @retval 0 Not yet.
 */
int sm_filter_detect_apogee(void);
#endif /* CONFIG_FILTER */

/*-----------------------------------------------------------
 * Remove-before-flight arm input (defined by state_rbf.c)
 *----------------------------------------------------------*/

#if defined(CONFIG_AURORA_STATE_MACHINE_RBF)
/**
 * @brief Bring up the remove-before-flight interlock GPIO.
 *
 * Called from @ref sm_init.  Latches the current pin level before enabling
 * the change interrupt, so a board that boots with the interlock already
 * pulled comes up armed.
 *
 * @retval 0 on success, negative errno otherwise.  On failure the interlock
 *         reports safe, holding the machine in @c SM_IDLE.
 */
int sm_rbf_init(void);

/**
 * @brief Debounced state of the interlock.
 *
 * The core substitutes this for @c sm_inputs.armed on every update.
 *
 * @retval true if the interlock has been removed (vehicle armed).
 * @retval false while it is installed, or before @ref sm_rbf_init succeeded.
 */
bool sm_rbf_armed(void);

/**
 * @brief force the interlock's current level as the armed state
 *
 * @ref sm_rbf_init deliberately leaves the vehicle reporting SAFE at boot and
 * waits for an edge, so that forgetting to install the plug cannot arm the
 * machine.
 * On cold boot this works, but a WDT recovery fails here.
 *
 * Only the flight-recovery path may call this, and only once it knows the
 * reset came from the watchdog and a valid in-flight record exists.
 *
 * @retval 0 on success.
 * @retval -errno as reported by the GPIO driver.
 */
int sm_rbf_resync(void);
#endif /* CONFIG_AURORA_STATE_MACHINE_RBF */

/*-----------------------------------------------------------
 * Backend -> common core (defined by the selected backend)
 *----------------------------------------------------------*/

/**
 * @brief Load @p cfg and initialise backend-private resources (timers, ...).
 *
 * @param cfg Threshold configuration to copy into the backend.
 */
void sm_backend_init(const struct sm_thresholds *cfg);

/**
 * @brief Replace the loaded thresholds, leaving timers and state alone.
 *
 * @param cfg Threshold configuration to copy into the backend.
 */
void sm_backend_set_thresholds(const struct sm_thresholds *cfg);

/**
 * @brief Release backend-private resources and clear loaded thresholds.
 */
void sm_backend_deinit(void);

/**
 * @brief Stop and clear all backend timers.
 *
 * Called by the common error path when recovering to @c SM_IDLE.
 */
void sm_backend_stop_timers(void);

/**
 * @brief Evaluate @p in against the flight logic and drive transitions.
 *
 * @param in                Current (already filtered, if enabled) inputs.
 * @param previous_altitude Altitude from the previous update cycle (m).
 */
void sm_backend_step(const struct sm_inputs *in, double previous_altitude);

#endif /* APP_LIB_STATE_INTERNAL_H_ */
