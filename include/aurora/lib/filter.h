/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_FILTER_H_
#define APP_LIB_FILTER_H_

/**
* @defgroup lib_filter library
* @ingroup lib
* @{
*
* @brief AURORA FILTER library.
*
* This library contains filter functions for apogee detection.
*/

#define FILTER_SCALE_DIVISOR 1000.0f

/**
 * @brief 2-state filter structure for rocket apogee detection
 *
 * State vector:
 *      x[0] = altitude (m)
 *      x[1] = vertical velocity (m/s)
 *
 * The filter assumes a constant velocity model and uses
 * barometric altitude as the measurement input.
 *
 * Designed for embedded systems (no dynamic allocation).
 */
struct filter {
    float state[2];
    float covariance[2][2];
    float noise_p[2][2];
    float noise_m;
};

/**
 * @brief Initialize filter
 *
 * Initializes state, covariance, and tuning parameters.
 *
 * @param filter Pointer to filter structure
 *
 * @retval 0 on success
 */
int filter_init(struct filter *filter);

/**
 * @brief Perform filter prediction step
 *
 * Propagates state and covariance forward in time.
 *
 * @param filter Pointer to filter structure
 * @param dt time in nsec since the last tick
 *
 * @retval 0 on success, negative error code on failure.
 */
int filter_predict(struct filter *filter, int64_t dt);

/**
 * @brief Perform filter update step using altitude measurement
 *
 * Updates state estimate using barometric altitude.
 *
 * @param filter Pointer to filter structure
 * @param z      Measured altitude (meters)
 *
 * @retval 0 on success, negative error code on failure.
 */
int filter_update(struct filter *filter, float z);

/**
 * @brief Detect apogee
 *
 * Function detects the apogee state
 *
 * @param filter Pointer to the filter structure
 *
 * @retval 0 on success, negative error code on failure.
 */
int filter_detect_apogee(struct filter *filter);

/** @} */

#endif /* APP_LIB_FILTER_H_ */
