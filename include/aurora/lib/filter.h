/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_FILTER_H_
#define APP_LIB_FILTER_H_

#include <inttypes.h>

/**
 * @defgroup lib_filter Input filter
 * @ingroup lib
 * @{
 *
 * @brief Kalman filter library for state machine.
 */

/** @brief Divisor applied to Kconfig milliscale noise parameters. */
#define FILTER_SCALE_DIVISOR 1000.0

/**
 * @brief 2-state Kalman filter structure for rocket state machine.
 *
 * State vector:
 *      x[0] = altitude (m)
 *      x[1] = vertical velocity (m/s)
 *
 * The filter uses a constant-acceleration model with vertical acceleration
 * supplied as a control input to filter_predict(), and barometric altitude
 * as the measurement input.  Passing a_vert = 0.0 reduces the model to
 * constant-velocity behavior.
 */
struct filter {
    double state[2];         /**< State vector [altitude, velocity]. */
    double covariance[2][2]; /**< State covariance matrix P. */
    double noise_p[2][2];    /**< Process noise covariance Q. */
    double noise_m;          /**< Measurement noise variance R. */

    /* Multi-criterion apogee detection state. */
    double peak_altitude;    /**< Highest altitude estimate seen so far (m). */
    double last_accel_vert;  /**< Most recent world-vertical accel (m/s^2). */
    int consecutive_apogee;  /**< Count of consecutive samples meeting criteria. */
    int apogee_latched;      /**< Non-zero once apogee has been reported. */
};

/**
 * @brief Initialize the filter.
 *
 * Zeroes state, sets initial covariance, and loads process/measurement
 * noise from Kconfig (scaled by @ref FILTER_SCALE_DIVISOR).
 *
 * @param filter Pointer to filter structure.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p filter is NULL.
 */
int filter_init(struct filter *filter);

/**
 * @brief Perform the filter prediction step.
 *
 * Propagates state and covariance forward in time using a
 * constant-acceleration model with @p a_vert as the control input.
 *
 * @param filter Pointer to filter structure.
 * @param dt     Elapsed time in nanoseconds since last prediction.
 * @param a_vert World-frame vertical acceleration (m/s^2), gravity-removed.
 *               Pass 0.0 to fall back to constant-velocity behavior when
 *               no acceleration input is available.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p filter is NULL or @p dt <= 0.
 */
int filter_predict(struct filter *filter, int64_t dt, double a_vert);

/**
 * @brief Perform the filter measurement update step.
 *
 * Computes Kalman gain and corrects the state estimate using a
 * barometric altitude measurement.
 *
 * @param filter Pointer to filter structure.
 * @param z      Measured altitude in meters.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p filter is NULL.
 */
int filter_update(struct filter *filter, double z);

/**
 * @brief Detect apogee using a multi-criterion vote.
 *
 * Combines three independent signals to reject noise-driven false
 * positives:
 *   1. Filtered vertical velocity is non-positive.
 *   2. Filtered altitude has descended at least
 *      @c CONFIG_FILTER_APOGEE_DELTA_H_CM below the tracked peak.
 *   3. Most recent world-frame vertical acceleration (passed to
 *      @ref filter_predict as @p a_vert) is below
 *      @c CONFIG_FILTER_APOGEE_ACCEL_MAX_MILLI, rejecting thrust
 *      or vibration spikes.
 *
 * All three conditions must hold for
 * @c CONFIG_FILTER_APOGEE_DEBOUNCE_SAMPLES consecutive calls before
 * apogee is reported.  Apogee is latched – once reported, subsequent
 * calls return 0 until @ref filter_init is called again.
 *
 * @param filter Pointer to filter structure.
 *
 * @retval 1 if apogee detected.
 * @retval 0 if apogee not detected.
 * @retval -EINVAL if @p filter is NULL.
 */
int filter_detect_apogee(struct filter *filter);

/** @} */

#endif /* APP_LIB_FILTER_H_ */
