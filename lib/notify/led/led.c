/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/notify.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(notify_led, CONFIG_AURORA_NOTIFY_LOG_LEVEL);

static const struct pwm_dt_spec led =
	PWM_DT_SPEC_GET(DT_CHOSEN(auxspace_led));

/** @brief Play a tone for @p duration_ms then silence. */
static int blink(uint32_t period_ns, uint32_t duration_ms)
{
	int ret;

	ret = pwm_set_dt(&led, period_ns, period_ns / 2);
	if (ret) {
		return ret;
	}
	k_sleep(K_MSEC(duration_ms));
	return pwm_set_dt(&led, period_ns, 0);
}

static int led_init(void)
{
	if (!pwm_is_ready_dt(&led)) {
		LOG_ERR("Buzzer PWM device not ready");
		return -ENODEV;
	}
	return 0;
}

static int led_on_boot(void)
{
	return blink(PWM_HZ(4000), 500);
}

static int led_on_state_change(enum sm_state prev, enum sm_state next)
{
	ARG_UNUSED(prev);

	switch (next) {
	case SM_ARMED:
		return blink(PWM_HZ(2000), 200);
	case SM_IDLE:
		return blink(PWM_HZ(1000), 100);
	case SM_APOGEE:
		return blink(PWM_HZ(3000), 300);
	case SM_LANDED:
		return blink(PWM_HZ(1000), 1000);
	default:
		return 0;
	}
}

static int led_on_error(void)
{
	/* Three short high-pitched beeps. */
	for (int i = 0; i < 3; i++) {
		int ret = blink(PWM_HZ(4000), 100);

		if (ret) {
			return ret;
		}
		k_sleep(K_MSEC(100));
	}
	return 0;
}

static const struct notify_backend_api led_api = {
	.init = led_init,
	.on_boot = led_on_boot,
	.on_state_change = led_on_state_change,
	.on_error = led_on_error,
};

NOTIFY_BACKEND_DEFINE(notify_led, &led_api);
