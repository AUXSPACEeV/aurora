/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BARO_H_
#define APP_LIB_BARO_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

/**
 * @defgroup lib_baro Barometer library
 * @ingroup lib
 * @{
 *
 * @brief AURORA barometer library for avionics telemetry.
 */


/** ZBUS channel for baro data. */
ZBUS_CHAN_DECLARE(baro_data_chan);

/**
 * @brief baro measurement data structure.
 *
 * carries the measurement data from the baro including temperature and
 * pressure readings. This struct is used as a
 * z-bus message payload for baro data updates
 */
struct baro_data
{
	struct sensor_value temperature; /**< Latest temperature reading */
	struct sensor_value pressure;  /**< Latest pressure reading */
};

#if !defined(CONFIG_BARO_TRIGGER)
/**
 * @brief Measure temperature and pressure from the barometric sensor and
 * publish the data to the z-bus.
 *
 * @param dev   Pointer to the barometric sensor device.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p dev is NULL
 * @retval -errno Other negative errno on failure.
 */
int baro_measure(const struct device *dev);
#endif /* CONFIG_BARO_TRIGGER */

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
int baro_set_reference(float ref_kpa);

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
float baro_pressure_to_altitude(float press_kpa);

/** @} */

#endif /* APP_LIB_BARO_H_ */
