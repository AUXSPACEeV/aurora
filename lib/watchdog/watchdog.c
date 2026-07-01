/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/watchdog.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

LOG_MODULE_REGISTER(watchdog, CONFIG_AURORA_WATCHDOG_LOG_LEVEL);

#define WDT_NODE DT_CHOSEN(auxspace_wdt)
BUILD_ASSERT(DT_NODE_HAS_STATUS(WDT_NODE, okay),
	     "chosen auxspace,wdt is missing or disabled");

/** @brief One supervised task slot. Protected by @ref lock. */
struct wdt_task {
	const char *name;
	uint32_t deadline_ms;
	int64_t last_feed; /**< uptime (ms) of the most recent check-in */
	bool in_use;
};

static const struct device *const wdt_dev = DEVICE_DT_GET(WDT_NODE);
static int wdt_channel;

static struct wdt_task tasks[CONFIG_AURORA_WATCHDOG_MAX_TASKS];
static struct k_spinlock lock;

/* Name of the task that last missed its deadline, for the expiry callback.
 * Written by the monitor thread, read from ISR context.
 */
static const char *volatile stale_task;

static K_THREAD_STACK_DEFINE(monitor_stack, CONFIG_AURORA_WATCHDOG_MONITOR_STACK_SIZE);
static struct k_thread monitor_thread;

/**
 * @brief Hardware watchdog pre-reset (interrupt) handler.
 *
 * Only invoked on controllers whose driver supports a warning stage before the
 * reset (e.g. the ESP32 MWDT, which fires this one timeout period before the
 * SoC reset). Runs in ISR context; flush the logs synchronously so the culprit
 * reaches the console. Reset-only controllers (e.g. the RP2040/RP2350
 * pico-watchdog) never call this — they reset directly with no warning.
 */
static void wdt_expiry_cb(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);

	LOG_ERR("hardware watchdog expired (stalled task: %s); resetting SoC",
		stale_task ? stale_task : "unknown");
	LOG_PANIC();
}

/**
 * @brief Supervisor loop: feed the hardware watchdog only while all tasks are healthy.
 */
static void wdt_monitor(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		k_sleep(K_MSEC(CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS));

		int64_t now = k_uptime_get();
		const char *culprit = NULL;

		k_spinlock_key_t key = k_spin_lock(&lock);
		for (int i = 0; i < CONFIG_AURORA_WATCHDOG_MAX_TASKS; i++) {
			struct wdt_task *t = &tasks[i];

			if (!t->in_use) {
				continue;
			}
			if ((now - t->last_feed) > (int64_t)t->deadline_ms) {
				culprit = t->name;
				break;
			}
		}
		k_spin_unlock(&lock, key);

		if (culprit == NULL) {
			/* All registered tasks healthy (or none yet): keep the
			 * hardware watchdog satisfied.
			 */
			wdt_feed(wdt_dev, wdt_channel);
		} else {
			/* Withhold the feed. The hardware watchdog then fires
			 * wdt_expiry_cb (if the driver has a warning stage) and
			 * resets the SoC.
			 */
			stale_task = culprit;
			LOG_ERR("task '%s' missed its watchdog deadline; "
				"withholding feed -> reset imminent", culprit);
		}
	}
}

int aurora_wdt_init(void)
{
	int ret;

	if (!device_is_ready(wdt_dev)) {
		LOG_ERR("watchdog device %s not ready", wdt_dev->name);
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.flags = WDT_FLAG_RESET_SOC,
		.window = {
			.min = 0U,
			.max = CONFIG_AURORA_WATCHDOG_TIMEOUT_MS,
		},
		.callback = wdt_expiry_cb,
	};

	ret = wdt_install_timeout(wdt_dev, &cfg);
	if (ret == -ENOTSUP) {
		/* Reset-only controllers (e.g. the RP2040/RP2350 pico-watchdog)
		 * reject a pre-reset callback. Retry without it; supervision
		 * still works, we just lose the pre-reset log line.
		 */
		cfg.callback = NULL;
		ret = wdt_install_timeout(wdt_dev, &cfg);
	}
	if (ret < 0) {
		LOG_ERR("wdt_install_timeout failed: %d", ret);
		return ret;
	}
	wdt_channel = ret;

	/* Pause while a debugger has the core halted so breakpoints don't
	 * trigger a spurious reset during bring-up.
	 */
	ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret < 0) {
		LOG_ERR("wdt_setup failed: %d", ret);
		return ret;
	}

	k_thread_create(&monitor_thread, monitor_stack,
			K_THREAD_STACK_SIZEOF(monitor_stack), wdt_monitor,
			NULL, NULL, NULL,
			CONFIG_AURORA_WATCHDOG_MONITOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&monitor_thread, "wdt_monitor");

	LOG_INF("hardware watchdog armed: %d ms timeout, feeding every %d ms",
		CONFIG_AURORA_WATCHDOG_TIMEOUT_MS,
		CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS);
	return 0;
}

aurora_wdt_task_t aurora_wdt_register(const char *name, uint32_t deadline_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	for (int i = 0; i < CONFIG_AURORA_WATCHDOG_MAX_TASKS; i++) {
		if (!tasks[i].in_use) {
			tasks[i].in_use = true;
			tasks[i].name = name;
			tasks[i].deadline_ms = deadline_ms;
			tasks[i].last_feed = k_uptime_get();
			k_spin_unlock(&lock, key);

			LOG_INF("registered task '%s' (deadline %u ms) in slot %d",
				name, deadline_ms, i);
			return i;
		}
	}

	k_spin_unlock(&lock, key);
	LOG_ERR("no free watchdog slot for task '%s' "
		"(increase CONFIG_AURORA_WATCHDOG_MAX_TASKS)", name);
	return AURORA_WDT_TASK_INVALID;
}

int aurora_wdt_feed(aurora_wdt_task_t task)
{
	if (task < 0 || task >= CONFIG_AURORA_WATCHDOG_MAX_TASKS) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	if (!tasks[task].in_use) {
		k_spin_unlock(&lock, key);
		return -EINVAL;
	}
	tasks[task].last_feed = k_uptime_get();
	k_spin_unlock(&lock, key);

	return 0;
}

int aurora_wdt_unregister(aurora_wdt_task_t task)
{
	if (task < 0 || task >= CONFIG_AURORA_WATCHDOG_MAX_TASKS) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	tasks[task].in_use = false;
	k_spin_unlock(&lock, key);

	return 0;
}
