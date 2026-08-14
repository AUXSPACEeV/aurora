/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file watchdog.c
 * @brief Hardware watchdog supervision of kernel liveness.
 *
 * See watchdog.h for what this deliberately does and does not supervise.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

#include <aurora/lib/watchdog.h>

LOG_MODULE_REGISTER(aurora_watchdog, CONFIG_AURORA_WATCHDOG_LOG_LEVEL);

/* Build-time dependency: the board must expose a hardware watchdog as
 * watchdog0, otherwise the task watchdog has nothing to fall back on and
 * the supervisor shares the fate of whatever wedged the kernel. */
#if !DT_HAS_ALIAS(watchdog0)
#error "CONFIG_AURORA_WATCHDOG requires a 'watchdog0' alias pointing at a hardware watchdog."
#endif

static const struct device *const hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static int wdt_channel = -1;

static void wdt_feeder(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		(void)task_wdt_feed(wdt_channel);
		k_sleep(K_MSEC(CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS));
	}
}

K_THREAD_DEFINE(aurora_wdt_feeder, CONFIG_AURORA_WATCHDOG_STACK_SIZE,
		wdt_feeder, NULL, NULL, NULL,
		CONFIG_AURORA_WATCHDOG_PRIORITY, 0, K_TICKS_FOREVER);

/* aurora_watchdog_setup - see watchdog.h */
int aurora_watchdog_setup(void)
{
	int rc;

	if (!device_is_ready(hw_wdt)) {
		LOG_ERR("hardware watchdog %s not ready; kernel liveness is "
			"unsupervised", hw_wdt->name);
		return -ENODEV;
	}

	rc = task_wdt_init(hw_wdt);
	if (rc != 0) {
		LOG_ERR("task watchdog init failed (%d)", rc);
		return rc;
	}

	rc = task_wdt_add(CONFIG_AURORA_WATCHDOG_TIMEOUT_MS, NULL, NULL);
	if (rc < 0) {
		LOG_ERR("could not register watchdog channel (%d)", rc);
		return rc;
	}
	wdt_channel = rc;

	k_thread_start(aurora_wdt_feeder);

	LOG_INF("watchdog armed: %d ms timeout, fed every %d ms",
		CONFIG_AURORA_WATCHDOG_TIMEOUT_MS,
		CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS);

	return 0;
}
