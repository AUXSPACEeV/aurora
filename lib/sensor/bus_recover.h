/**
 * @file bus_recover.h
 * @brief Shared I2C bus-unwedge helper for the sensor wrappers.
 *
 * A slave that is glitched mid-byte can leave SDA held low.
 * The i.e. ESP32 controller then reports the bus as permanently busy,
 * and i2c_esp32_transfer() answers every subsequent transfer with -EBUSY only
 * after burning @c I2C_TRANSFER_TIMEOUT_MSEC (500 ms) in a non-yielding
 * @c k_busy_wait loop.  On the IMU that loop runs on the LSM6DSO
 * trigger thread, which Zephyr creates at @c K_PRIO_COOP().  So it
 * cannot be preempted, and at the sensor ODR the board stops making
 * forward progress entirely.
 *
 * @c i2c_recover_bus() clocks the bus until the slave releases SDA but nothing
 * calls it automatically.
 * This helper wires it into the sensor error paths, rate-limited so a
 * genuinely dead bus does not turn into a recovery spam.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_SENSOR_BUS_RECOVER_H_
#define AURORA_LIB_SENSOR_BUS_RECOVER_H_

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

/** Minimum spacing between recovery attempts on one bus. */
#define AURORA_BUS_RECOVER_MIN_INTERVAL_MS 1000

/**
 * @brief Attempt to unwedge an I2C bus after a failed sensor transfer.
 *
 * Only acts on the error codes that indicate the bus itself is stuck
 * (@c -EBUSY, @c -ETIMEDOUT, @c -EIO, @c -ECANCELED); anything else is a
 * sensor-level problem that clocking the bus cannot fix.
 *
 * @param bus       I2C controller the sensor hangs off, or NULL to skip.
 * @param err       errno returned by the failed transfer.
 * @param last_ms   Caller-owned timestamp of the previous attempt,
 *                  updated on each recovery. Rate-limits the retries.
 * @retval true  Recovery was attempted and reported success.
 * @retval false Not attempted, or the bus is still wedged.
 */
static inline bool aurora_i2c_bus_recover(const struct device *bus, int err,
					  int64_t *last_ms)
{
	if (bus == NULL || last_ms == NULL) {
		return false;
	}

	if (err != -EBUSY && err != -ETIMEDOUT && err != -EIO &&
	    err != -ECANCELED) {
		return false;
	}

	int64_t now = k_uptime_get();

	if ((now - *last_ms) < AURORA_BUS_RECOVER_MIN_INTERVAL_MS) {
		return false;
	}
	*last_ms = now;

	return i2c_recover_bus(bus) == 0;
}

#endif /* AURORA_LIB_SENSOR_BUS_RECOVER_H_ */
