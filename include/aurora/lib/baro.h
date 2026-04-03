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
typedef struct
{
    struct sensor_value temperature; /**< Latest temperature reading */
    struct sensor_value pressure;  /**< Latest pressure reading */
}baro_data_t;

#if !defined(CONFIG_LPS22HH_TRIGGER)
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
#endif /* CONFIG_LPS22HH_TRIGGER */

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
 * @brief Calculate the altitude AGL from a pressure reading.
 *
 * Uses the hypsometric formula (ISA troposphere model) with the
 * reference pressure set by @ref baro_set_reference.
 *
 * @param press_kpa Measured pressure in kilopascals.
 *
 * @return Altitude in meters above the reference level.
 */
int baro_sensor_value_to_altitude(const struct sensor_value *press, float *altitude_out);

/** @} */

#endif /* APP_LIB_BARO_H_ */
