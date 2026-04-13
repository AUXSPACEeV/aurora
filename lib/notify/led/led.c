/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/notify.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(notify_led, CONFIG_AURORA_NOTIFY_LOG_LEVEL);

#define MAX_BRIGHTNESS 100

#define LED_PWM_NODE_ID	 DT_CHOSEN(auxspace_led)

static const struct device *pwm_leds = DEVICE_DT_GET(LED_PWM_NODE_ID);
static const char *led_labels[] = {
	DT_FOREACH_CHILD_SEP_VARGS(LED_PWM_NODE_ID, DT_PROP_OR, (,), label, NULL)
};
const int num_leds = ARRAY_SIZE(led_labels);

/** @brief Blink with @p delay_on and @p delay_off in ms. */
static int blink(uint32_t delay_on, uint32_t delay_off)
{
	int rc = 0;
	for (int led = 0; led < num_leds; led++) {
		int err = led_blink(pwm_leds, led, delay_on, delay_off);
		if (err < 0) {
			LOG_ERR("Could not blink %s (%d)", led_labels[led], err);
			rc = err;
		}
	}
	return rc;
}

/** @brief Turn on all LEDs with brightness @p level */
static int all_leds_on(int level)
{
	int rc = 0;

	if (level > MAX_BRIGHTNESS) {
		LOG_ERR("Brightness level too high (%d > %d)",
			level, MAX_BRIGHTNESS);
		return -EINVAL;
	}

	for (int led = 0; led < num_leds; led++) {
		int err = led_on(pwm_leds, led);
		if (err < 0) {
			LOG_ERR("Could not turn %s on (%d)", led_labels[led], err);
			rc = err;
		}
		err = led_set_brightness(pwm_leds, led, level);
		if (err < 0) {
			LOG_ERR("Could not set LED %s brightness to %d (%d)",
				led_labels[led], level, err);
			rc = err;
		}
	}
	return rc;
}

/** @brief Turn off all LEDs */
static int all_leds_off(void)
{
	int rc = 0;
	for (int led = 0; led < num_leds; led++) {
		int err = led_off(pwm_leds, led);
		if (err < 0) {
			LOG_ERR("Could not turn %s off (%d)", led_labels[led], err);
			rc = err;
		}
	}
	return rc;
}

/** @brief Initialize all LEDs */
static int led_init(void)
{
	if (!device_is_ready(pwm_leds)) {
		LOG_ERR("Device %s is not ready", pwm_leds->name);
		return -ENODEV;
	}

	if (!num_leds) {
		LOG_ERR("No LEDs found for %s", pwm_leds->name);
		return 0;
	}

	return 0;
}

static int led_on_boot(void)
{
	int rc = 0;
	int err = 0;

	err = all_leds_on(MAX_BRIGHTNESS);
	if (err)
		rc = err;

	k_sleep(K_MSEC(200));

	err = all_leds_off();
	if (err)
		rc = err;

	return rc;
}

static int led_on_error(void)
{
	return all_leds_on(MAX_BRIGHTNESS);
}

static int led_on_state_change(enum sm_state prev, enum sm_state next)
{
	ARG_UNUSED(prev);

	switch (next) {
	case SM_ARMED:
		return blink(200, 200);
	case SM_IDLE:
		return blink(50, 450);
	case SM_LANDED:
		return blink(400, 100);
	case SM_ERROR:
		return led_on_error();
	default:
		return all_leds_off();
	}
}

static const struct notify_backend_api led_api = {
	.init = led_init,
	.on_boot = led_on_boot,
	.on_state_change = led_on_state_change,
	.on_error = led_on_error,
};

NOTIFY_BACKEND_DEFINE(notify_led, &led_api);
