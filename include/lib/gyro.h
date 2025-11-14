/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_GYRO_H_
#define APP_LIB_GYRO_H_

#include <zephyr/device.h>


/**
* @defgroup lib_gyro library
* @ingroup lib
* @{
*
* @brief AURORA gyroscope library for avionics telemetry.
*
* This library contains gyroscope functions.
*/

/**
* @brief Initialize the gyro for the AURORA application.
*
* Function initializes the gyro and returns 0 on success.
*
* @retval 0 on success, negative error code on failure.
*/
int gyro_init(const struct device *dev);

/** @} */

#endif /* APP_LIB_GYRO_H_ */
