/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_HACCEL_H_
#define APP_LIB_HACCEL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

/**
 * @defgroup lib_haccel High-g accelerometer library
 * @ingroup lib
 * @{
 *
 * @brief AURORA high-g accelerometer library for avionics telemetry.
 *
 * Covers the range the IMU accelerometer saturates in. Its samples are
 * logged, but not yet used by the state machine.
 */

/** Number of axes for high-g accelerometer measurements. */
#define HACCEL_NUM_AXES 3

/** ZBUS channel for high-g accelerometer data. */
ZBUS_CHAN_DECLARE(haccel_data_chan);

/**
 * @brief High-g accelerometer measurement data structure.
 *
 * Carries one sample of the x, y and z axes in m/s^2. This struct is used as
 * a z-bus message payload for high-g accelerometer data updates.
 */
struct haccel_data
{
	struct sensor_value accel[HACCEL_NUM_AXES]; /**< Acceleration (x, y, z) in m/s^2. */
};

/**
 * @brief Take one high-g accelerometer sample, publish it on the z-bus and
 *        hand it back.
 *
 * @param dev Pointer to the high-g accelerometer device.
 * @param out Optional; receives a copy of the sample.
 *
 * @retval 0 on success.
 * @retval -errno Negative errno on failure.
 */
int haccel_poll(const struct device *dev, struct haccel_data *out);

/**
 * @brief Initialize the high-g accelerometer.
 *
 * @param dev Pointer to the high-g accelerometer device.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p dev is NULL.
 * @retval -ENODEV if the device is not ready.
 */
int haccel_init(const struct device *dev);

/** @} */

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_haccel_data(const struct haccel_data *haccel);
#else
static inline void log_haccel_data(const struct haccel_data *haccel) {}
#endif
#endif /* APP_LIB_HACCEL_H_ */
