/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_IMU_H_
#define APP_LIB_IMU_H_

#include <zephyr/device.h>


/**
* @defgroup lib_imu library
* @ingroup lib
* @{
*
* @brief AURORA IMU library for avionics telemetry.
*
* This library contains IMU functions.
*/

#if !defined(CONFIG_LSM6DSO_TRIGGER)
/**
 * @brief Poll the IMU for new data.
 * 
 * Function fetches new data from the IMU device.
 *
 * @param dev Pointer to the IMU device
 *
 * @retval 0 on success, negative error code on failure.
 */
int imu_poll(const struct device *dev);
#endif /* CONFIG_LSM6DSO_TRIGGER */

/**
 * @brief Set the IMU sampling frequency.
 * 
 * Function sets the sampling frequency of the IMU device.
 * 
 * @param dev Pointer to the IMU device
 * 
 * @param sampling_rate_hz The desired sampling rate in Hertz.
 * 
 * @retval 0 on success, negative error code on failure.
 */
int imu_set_sampling_freq(const struct device *dev, float sampling_rate_hz);

/**
* @brief Initialize the IMU for the AURORA application.
*
* Function initializes the IMU and returns 0 on success.
*
* @retval 0 on success, negative error code on failure.
*/
int imu_init(const struct device *dev);

/** @} */

#endif /* APP_LIB_IMU_H_ */
