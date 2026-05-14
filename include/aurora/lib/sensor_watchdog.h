/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_SENSOR_WATCHDOG_H_
#define APP_LIB_SENSOR_WATCHDOG_H_

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>

/**
 * @defgroup lib_sensor_watchdog Sensor stall watchdog
 * @ingroup lib
 * @{
 *
 * @brief Generic publish-stall watchdog for trigger-mode sensors.
 *
 * Sensors that publish their readings via zbus on a hardware data-ready
 * interrupt can silently stop producing samples if the chip soft-resets
 * (e.g. from a power-rail transient). This helper records the time of the
 * most recent publish and, on a periodic check, invokes a sensor-specific
 * recovery callback when the publish age exceeds a timeout.
 *
 * Usage pattern:
 *   1. Define a static @ref sensor_watchdog with timeout, period, and a
 *      sensor-specific @c recover callback.
 *   2. After the device init succeeds, set @c dev and call
 *      @ref sensor_watchdog_init to seed the publish clock.
 *   3. Call @ref sensor_watchdog_mark_publish on every successful publish.
 *   4. Have a thread call @ref sensor_watchdog_run, which never returns.
 */

/**
 * @brief Watchdog state and configuration for a single sensor.
 *
 * Configuration fields (@c name, @c dev, @c timeout_ms, @c period_ms,
 * @c recover) are written by the owner before @ref sensor_watchdog_init.
 * @c last_pub_ms is internal.
 */
struct sensor_watchdog {
	const char *name;                            /**< Used in log messages. */
	const struct device *dev;                    /**< Sensor device passed to @c recover. */
	uint32_t timeout_ms;                         /**< Stall threshold. */
	uint32_t period_ms;                          /**< Check cadence. */
	int (*recover)(const struct device *dev);    /**< Sensor-specific recovery. */
	atomic_t last_pub_ms;                        /**< Internal: last publish uptime (ms). */
};

/**
 * @brief Seed the publish clock.
 *
 * Call once after the underlying sensor has initialized. Without this seed
 * the watchdog would trip immediately on first run.
 */
void sensor_watchdog_init(struct sensor_watchdog *wd);

/**
 * @brief Record a successful publish.
 *
 * Safe to call from interrupt or workqueue context.
 */
void sensor_watchdog_mark_publish(struct sensor_watchdog *wd);

/**
 * @brief Milliseconds since the last @ref sensor_watchdog_mark_publish call.
 *
 * Wrap-safe over @c k_uptime_get_32. Returns time-since-init until the first
 * publish.
 */
uint32_t sensor_watchdog_age_ms(const struct sensor_watchdog *wd);

/**
 * @brief Watchdog thread entry. Never returns.
 *
 * Sleeps @c period_ms, checks the publish age, calls @c recover when the age
 * exceeds @c timeout_ms, and logs the outcome.
 */
void sensor_watchdog_run(struct sensor_watchdog *wd);

/** @} */

#endif /* APP_LIB_SENSOR_WATCHDOG_H_ */
