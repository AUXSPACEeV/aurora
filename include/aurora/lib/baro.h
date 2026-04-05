/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BARO_H_
#define APP_LIB_BARO_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

/**
 * @defgroup lib_baro Barometer library
 * @ingroup lib
 * @{
 *
 * @brief AURORA barometer library for avionics telemetry.
 */

/**
 * @brief Measure temperature and pressure from the barometric sensor.
 *
 * @param dev   Pointer to the barometric sensor device.
 * @param temp  Output for temperature, or NULL to skip.
 * @param press Output for pressure, or NULL to skip.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p dev is NULL and both outputs are NULL.
 * @retval -errno Other negative errno on failure.
 */
int baro_measure(const struct device *dev, struct sensor_value *temp,
				 struct sensor_value *press);

/**
 * @brief Initialize the barometric pressure sensor.
 *
 * Checks device readiness and configures the oversampling rate.
 *
 * @param dev Pointer to the barometric sensor device.
 *
 * @retval 0 on success.
 * @retval -ETIMEDOUT if the device is not ready.
 * @retval -EIO if oversampling configuration fails.
 */
int baro_init(const struct device *dev);

/**
 * @brief Set the ground-level reference pressure.
 *
 * Must be called before @ref baro_pressure_to_altitude to establish
 * the zero-altitude baseline.  Typically called once at startup with
 * the first valid pressure reading.
 *
 * @param ref_kpa Ground-level pressure in kilopascals.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p ref_kpa is not positive.
 */
int baro_set_reference(double ref_kpa);

/**
 * @brief Convert a pressure reading to altitude AGL.
 *
 * Uses the hypsometric formula (ISA troposphere model) with the
 * reference pressure set by @ref baro_set_reference.
 *
 * @param press_kpa Measured pressure in kilopascals.
 *
 * @return Altitude in meters above the reference level.
 */
double baro_pressure_to_altitude(double press_kpa);

/** @} */

#endif /* APP_LIB_BARO_H_ */
