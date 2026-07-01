/**
 * @file disk_led.c
 * @brief SD-card activity LED indicator.
 *
 * Concept and wiring: see the "SD-card activity LED" page in the library
 * documentation.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/disk_led.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(disk_led, CONFIG_AURORA_NOTIFY_LOG_LEVEL);

#if DT_HAS_CHOSEN(auxspace_disk_led)

static const struct gpio_dt_spec disk_led =
	GPIO_DT_SPEC_GET(DT_CHOSEN(auxspace_disk_led), gpios);

/* Set once the pin is configured; guards against activity before init.
 * Atomic since init and activity may run on different threads.
 */
static atomic_t disk_led_ready = ATOMIC_INIT(0);

/* Deferred switch-off, run on the system wrkq when the hold timer fires. */
static void disk_led_off_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_dt(&disk_led, 0);
}
static K_WORK_DELAYABLE_DEFINE(disk_led_off_work, disk_led_off_fn);

/* disk_led_activity - see disk_led.h */
void disk_led_activity(void)
{
	if (!atomic_get(&disk_led_ready)) {
		return;
	}

	(void)gpio_pin_set_dt(&disk_led, 1);

	/* Reschedule pushes the off-point forward, so sustained activity keeps
	 * the LED lit and a pause switches it off.
	 */
	(void)k_work_reschedule(&disk_led_off_work,
				K_MSEC(CONFIG_AURORA_NOTIFY_DISK_LED_HOLD_MS));
}

static int disk_led_init(void)
{
	if (!gpio_is_ready_dt(&disk_led)) {
		LOG_ERR("SD activity LED GPIO not ready");
		return -ENODEV;
	}

	int rc = gpio_pin_configure_dt(&disk_led, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		LOG_ERR("Could not configure SD activity LED (%d)", rc);
		return rc;
	}

	atomic_set(&disk_led_ready, 1);
	return 0;
}

/* Run at boot, after the GPIO driver is up. */
SYS_INIT(disk_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else /* !DT_HAS_CHOSEN(auxspace_disk_led) */

/* Enabled in Kconfig but no LED node in this board's devicetree. */
void disk_led_activity(void) {}

#endif /* DT_HAS_CHOSEN(auxspace_disk_led) */
