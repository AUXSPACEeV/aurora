/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_FILTER_H_
#define APP_LIB_FILTER_H_

/**
 * @defgroup lib_filter Apogee detection filter
 * @ingroup lib
 * @{
 *
 * @brief Kalman filter library for rocket apogee detection.
 */

/** @brief Divisor applied to Kconfig milliscale noise parameters. */
#define FILTER_SCALE_DIVISOR 1000.0f

/**
 * @brief 2-state Kalman filter structure for rocket apogee detection.
 *
 * State vector:
 *      x[0] = altitude (m)
 *      x[1] = vertical velocity (m/s)
 *
 * The filter assumes a constant-velocity model and uses
 * barometric altitude as the measurement input.
 */
struct filter {
    float state[2];         /**< State vector [altitude, velocity]. */
    float covariance[2][2]; /**< State covariance matrix P. */
    float noise_p[2][2];    /**< Process noise covariance Q. */
    float noise_m;          /**< Measurement noise variance R. */
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
 * constant-velocity model.
 *
 * @param filter Pointer to filter structure.
 * @param dt     Elapsed time in nanoseconds since last prediction.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p filter is NULL or @p dt <= 0.
 */
int filter_predict(struct filter *filter, int64_t dt);

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
int filter_update(struct filter *filter, float z);

/**
 * @brief Detect apogee from the filter's velocity estimate.
 *
 * Returns 1 when the estimated vertical velocity crosses zero from
 * positive to non-positive, indicating apogee.
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
