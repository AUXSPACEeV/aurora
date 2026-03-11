/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_IMU_H_
#define APP_LIB_IMU_H_

#include <zephyr/device.h>


/**
 * @defgroup lib_imu IMU library
 * @ingroup lib
 * @{
 *
 * @brief AURORA IMU library for avionics telemetry.
 */

#if !defined(CONFIG_LSM6DSO_TRIGGER)
/**
 * @brief Poll the IMU for orientation and acceleration data.
 *
 * @param dev             Pointer to the IMU device.
 * @param orientation_deg Output for orientation angle in degrees, or NULL.
 * @param acc             Output for acceleration magnitude in m/s^2, or NULL.
 *
 * @retval 0 on success.
 * @retval -errno Negative errno on failure.
 */
int imu_poll(const struct device *dev, float *orientation_deg, float *acc);
#endif /* CONFIG_LSM6DSO_TRIGGER */

/**
 * @brief Set the IMU accelerometer and gyroscope sampling frequency.
 *
 * @param dev              Pointer to the IMU device.
 * @param sampling_rate_hz Desired sampling rate in Hertz.
 *
 * @retval 0 on success.
 * @retval -errno Negative errno on failure.
 */
int imu_set_sampling_freq(const struct device *dev, int sampling_rate_hz);

/**
 * @brief Initialize the IMU device.
 *
 * Checks device readiness and configures the sampling frequency.
 *
 * @param dev Pointer to the IMU device.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the device is not ready.
 */
int imu_init(const struct device *dev);

/** @} */

#endif /* APP_LIB_IMU_H_ */
