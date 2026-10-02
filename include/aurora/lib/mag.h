/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_MAG_H_
#define APP_LIB_MAG_H_

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

/**
 * @defgroup lib_mag Magnetometer library
 * @ingroup lib
 * @{
 *
 * @brief AURORA magnetometer library for avionics telemetry.
 *
 * Its samples are logged, but not yet used by the state machine.
 */

/** Number of axes for magnetometer measurements. */
#define MAG_NUM_AXES 3

/** ZBUS channel for magnetometer data. */
ZBUS_CHAN_DECLARE(mag_data_chan);

/**
 * @brief Magnetometer measurement data structure.
 *
 * Carries one sample of the x, y and z axes in gauss. This struct is used as
 * a z-bus message payload for magnetometer data updates.
 */
struct mag_data
{
	struct sensor_value field[MAG_NUM_AXES]; /**< Magnetic field (x, y, z) in gauss. */
};

/**
 * @brief Take one magnetometer sample, publish it on the z-bus and hand it
 *        back.
 *
 * A driver that converts in the background (CONFIG_MLX90395_PIPELINED)
 * reports a conversion still in progress as -EAGAIN, which is not an error:
 * the caller is polling faster than the sensor converts.
 *
 * @param dev Pointer to the magnetometer device.
 * @param out Optional; receives a copy of the sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN when no new sample is ready yet.
 * @retval -errno Negative errno on failure.
 */
int mag_poll(const struct device *dev, struct mag_data *out);

/**
 * @brief Initialize the magnetometer.
 *
 * @param dev Pointer to the magnetometer device.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p dev is NULL.
 * @retval -ENODEV if the device is not ready.
 */
int mag_init(const struct device *dev);

/** @} */

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_mag_data(const struct mag_data *mag);
#else
static inline void log_mag_data(const struct mag_data *mag) {}
#endif
#endif /* APP_LIB_MAG_H_ */
