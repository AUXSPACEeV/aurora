/*
 * Copyright (c) 2025 Auxspace e.V.
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

/** @} */

#endif /* APP_LIB_BARO_H_ */
