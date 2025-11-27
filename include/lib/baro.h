/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BARO_H_
#define APP_LIB_BARO_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

/**
* @defgroup lib_baro library
* @ingroup lib
* @{
*
* @brief AURORA barometer library for avionics telemetry.
*
* This library contains baro functions.
*/

/**
* @brief Measure temperature and pressure using the baro for the AURORA application.
*
* Function measures the baro and returns 0 on success.
*
* @retval 0 on success, negative error code on failure.
*/
int baro_measure(const struct device *dev, struct sensor_value *temp,
				 struct sensor_value *press);

/**
 * @brief Compute altitude from barometric pressure for the AURORA application.
 *
 * Function computes the approximate altitude (in meters) using the
 * standard barometric formula based on the measured pressure.
 *
 * @param pressure Measured barometric pressure in Pascals (Pa)
 *
 * @return Altitude in meters above sea level
 */
float baro_altitude(float pressure);

/** @} */

#endif /* APP_LIB_BARO_H_ */
