/**
 * @file sensor_watchdog.c
 * @brief Generic publish-stall watchdog for trigger-mode sensors.
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <aurora/lib/sensor_watchdog.h>

LOG_MODULE_REGISTER(sensor_watchdog, CONFIG_AURORA_SENSORS_LOG_LEVEL);

void sensor_watchdog_init(struct sensor_watchdog *wd)
{
	atomic_set(&wd->last_pub_ms, (atomic_val_t)k_uptime_get_32());
}

void sensor_watchdog_mark_publish(struct sensor_watchdog *wd)
{
	atomic_set(&wd->last_pub_ms, (atomic_val_t)k_uptime_get_32());
}

uint32_t sensor_watchdog_age_ms(const struct sensor_watchdog *wd)
{
	uint32_t last = (uint32_t)atomic_get(&wd->last_pub_ms);
	return k_uptime_get_32() - last;
}

void sensor_watchdog_run(struct sensor_watchdog *wd)
{
	while (1) {
		k_sleep(K_MSEC(wd->period_ms));

		uint32_t age_ms = sensor_watchdog_age_ms(wd);
		if (age_ms <= wd->timeout_ms) {
			continue;
		}

		LOG_WRN("%s stalled (%u ms since last sample); recovering",
			wd->name, age_ms);

		int rc = wd->recover ? wd->recover(wd->dev) : -ENOSYS;
		if (rc != 0) {
			LOG_ERR("%s recovery failed (%d)", wd->name, rc);
		} else {
			LOG_INF("%s recovery succeeded", wd->name);
			/* Restart the clock so the next miss is timed from
			 * here rather than from before the stall. The recover
			 * callback may also do this, but we make it explicit.
			 */
			sensor_watchdog_mark_publish(wd);
		}
	}
}
