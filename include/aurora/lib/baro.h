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

/**
 * @brief Take one baro sample, publish it on the z-bus and hand it back.
 *
 * In trigger mode the sample was already read off the sensor by the driver's
 * data-ready handler; this converts and publishes it. Returns -EAGAIN when no
 * new sample has arrived since the last call, which is not an error: the
 * caller is polling faster than the sensor produces.
 *
 * @param dev Pointer to the barometric sensor device. Ignored by the
 *            simulated sources.
 * @param out Optional; receives a copy of the sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN in trigger mode when no new sample is waiting.
 * @retval -errno Negative errno on failure.
 */
int baro_measure(const struct device *dev, struct baro_data *out);

/**
 * @brief Initialize the barometric pressure sensor.
 *
 * Checks device readiness and, with CONFIG_BARO_TRIGGER, installs the
 * library's own data-ready handler.
 *
 * A device that is present but cannot deliver that trigger reports
 * -ENOTSUP: the device is usable, and the caller is expected to fall back
 * to polling it with baro_measure() rather than treat the barometer as
 * absent.
 *
 * @param dev Pointer to the barometric sensor device. Ignored by the
 *            simulated sources.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p dev is NULL.
 * @retval -ENODEV if the device is not ready.
 * @retval -ENOTSUP if the device is ready but has no data-ready trigger.
 * @retval -ENODATA if a simulated source has no sample data.
 */
int baro_init(const struct device *dev);

/**
 * @brief Force the ground-level reference pressure to a known value.
 *
 * Overrides the reference unconditionally, including one already being
 * tracked.
 * Calling this is not required for normal operation:
 * @ref baro_sensor_value_to_altitude establishes the reference from the
 * first sample and keeps it on ambient by itself. It exists for callers
 * that know the true pad pressure independently (an operator entering a
 * QFE, a test fixture pinning a baseline).
 *
 * The value does not stay pinned. While the vehicle is on the pad the next
 * samples resume tracking from it, so a forced reference is a starting
 * point, not a latch. To hold one exactly, set it once the vehicle is no
 * longer on the pad.
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
 * Uses the hypsometric formula (ISA troposphere model) against the ground
 * reference pressure. The first call seeds that reference and subsequent
 * calls keep it low-pass tracked onto ambient for as long as the vehicle
 * is on the pad (see @c BARO_REF_TRACK_TAU_MS), then freeze it at liftoff.
 * So on the pad this reads ~0 m however far the sensor has drifted since
 * power-on, and in flight it reads height above the pad.
 *
 * @param press Barometric pressure as sensor_value.
 * @param altitude_out Altitude in meters above the reference level.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p press or @p altitude_out is NULL.
 * @retval -EDOM   if the pressure or the resulting altitude is not usable.
 */
int baro_sensor_value_to_altitude(const struct sensor_value *press, double *altitude_out);

/** @} */

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_baro_data(const struct baro_data *baro);
#else
static inline void log_baro_data(const struct baro_data *baro) {}
#endif
#endif /* APP_LIB_BARO_H_ */
