/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>

#include <app_version.h>

#if defined(CONFIG_SERVO)
#include <zephyr/drivers/pwm.h>
#endif /* CONFIG_SERVO */

#if defined(CONFIG_IMU)
#include <lib/imu.h>
#endif /* CONFIG_IMU */

LOG_MODULE_REGISTER(main, CONFIG_HUMMEL_CTRL_LOG_LEVEL);

#if defined(CONFIG_SERVO)
#define STEP PWM_USEC(100)
void servo_task(void *, void *, void *)
{
	const struct pwm_dt_spec servo = PWM_DT_SPEC_GET(DT_NODELABEL(servo0));
	const uint32_t min_pulse = DT_PROP(DT_NODELABEL(servo0), min_pulse);
	const uint32_t max_pulse = DT_PROP(DT_NODELABEL(servo0), max_pulse);
	enum direction {
		DOWN,
		UP,
	};

	uint32_t pulse_width = min_pulse;
	enum direction dir = UP;
	int ret;

	LOG_INF("Servomotor control\n");

	if (!pwm_is_ready_dt(&servo)) {
		LOG_ERR("Error: PWM device %s is not ready\n", servo.dev->name);
		return;
	}

	LOG_INF("Servomotor control started!\n");
	while (1) {
		ret = pwm_set_pulse_dt(&servo, pulse_width);
		if (ret < 0) {
			LOG_ERR("Error %d: failed to set pulse width\n", ret);
			return;
		}

		if (dir == DOWN) {
			if (pulse_width <= min_pulse) {
				dir = UP;
				pulse_width = min_pulse;
			} else {
				pulse_width -= STEP;
			}
		} else {
			pulse_width += STEP;

			if (pulse_width >= max_pulse) {
				dir = DOWN;
				pulse_width = max_pulse;
			}
		}
		k_sleep(K_SECONDS(1));
	}
}
/* Create the Servo task (inactive unless CONFIG_SERVO=y) */
K_THREAD_DEFINE(servo_task_id, 2048, servo_task, NULL, NULL, NULL,
				5, 0, 0);
#endif

/* ============================================================
 *                     IMU TASK
 * ============================================================ */
#if defined(CONFIG_IMU)
void imu_task(void *, void *, void *)
{
	static float orientation = 0.0f;
	static float acceleration = 0.0f;

	const struct device *imu0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_imu));
	const int imu_hz = CONFIG_IMU_FREQUENCY_VALUE;

	imu_init(imu0);

	while (1) {
		int rc = imu_poll(imu0, &orientation, &acceleration);
		if (rc != 0) {
			LOG_ERR("IMU polling failed (%d)", rc);
			break;
		}

		LOG_INF("orientation: %f deg. acc: %f\n", (double)orientation, (double)acceleration);

		k_sleep(K_MSEC(1000 / imu_hz));
	}

	LOG_INF("IMU task stopped.");
}

/* Create the IMU task (inactive unless CONFIG_IMU=y) */
K_THREAD_DEFINE(imu_task_id, 2048, imu_task, NULL, NULL, NULL,
				5, 0, 0);

#endif /* CONFIG_IMU */

/* ============================================================
 *                     MAIN INITIALIZATION
 * ============================================================ */
int main(void)
{
	LOG_INF("Auxspace AURORA - HUMMEL: %s", APP_VERSION_STRING);

	/* Threads start automatically via K_THREAD_DEFINE */

	return 0;
}
