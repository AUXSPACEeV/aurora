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
#include <zephyr/spinlock.h>

#include <app_version.h>

#if defined(CONFIG_SERVO)
#include <zephyr/drivers/pwm.h>
#endif /* CONFIG_SERVO */

#if defined(CONFIG_IMU)
#include <lib/imu.h>
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#include <lib/baro.h>
#endif /* CONFIG_BARO */

#if defined(CONFIG_AURORA_STATE_MACHINE)
#include <lib/state/state.h>

static const struct sm_thresholds state_cfg = {
	/* Sensor Metrics */
	.T_AB = CONFIG_BOOST_ACCELERATION,
	.T_H = CONFIG_BOOST_ALTITUDE,
	.T_BB = CONFIG_BURNOUT_ACCELERATION,
	.T_M = CONFIG_MAIN_DESCENT_HEIGHT,
	.T_L = CONFIG_LANDING_VELOCITY,
	.T_OA = CONFIG_ARM_ANGLE,
	.T_OI = CONFIG_DISARM_ANGLE,
	/* Timers */
	.DT_AB = CONFIG_BOOST_TIMER_MS,
	.DT_L = CONFIG_LANDING_TIMER_MS,

	/* Timeouts */
	.TO_A = CONFIG_APOGEE_TIMEOUT_MS,
	.TO_M = CONFIG_MAIN_TIMEOUT_MS,
	.TO_R = CONFIG_REDUNDAND_TIMEOUT_MS,
};
#endif /* CONFIG_AURORA_STATE_MACHINE */

LOG_MODULE_REGISTER(main, CONFIG_SENSOR_BOARD_LOG_LEVEL);

static float orientation = 0.0f;
static float acceleration = 0.0f;
static float velocity = 0.0f;
static float altitude = 0.0f;

static bool baro_active = false;
static bool imu_active = false;
static bool sm_active = false;

static struct k_spinlock sensor_lock;

static bool sensor_update = false;

#if defined(CONFIG_AURORA_SENSORS)
/* ============================================================
 *                     IMU TASK
 * ============================================================ */
#if defined(CONFIG_IMU)
void imu_task(void *, void *, void *)
{
	const struct device *imu0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_imu));
	const int imu_hz = CONFIG_IMU_FREQUENCY_VALUE;

	imu_init(imu0);
	imu_active = true;

	// int64_t lasttime = k_uptime_get();
	while (1) {
		int rc = imu_poll(imu0, &orientation, &acceleration);
		if (rc != 0) {
			LOG_ERR("IMU polling failed (%d)", rc);
			break;
		}

		K_SPINLOCK(&sensor_lock)
		{
			sensor_update = true;
		}

		LOG_INF("orientation: %f deg. acc: %f\n", (double)orientation, (double)acceleration);
		// int64_t delta = k_uptime_delta(&lasttime);
		// /* print the hz */
		// LOG_DBG("IMU Hz: %.2f", 1000.0 / delta);

		k_sleep(K_MSEC(1000 / imu_hz));
	}

	LOG_INF("IMU task stopped.");
}

/* Create the IMU task (inactive unless CONFIG_IMU=y) */
K_THREAD_DEFINE(imu_task_id, 2048, imu_task, NULL, NULL, NULL,
				5, 0, 0);

#endif /* CONFIG_IMU */

/* ============================================================
 *                     BARO TASK
 * ============================================================ */
#if defined(CONFIG_BARO)
void baro_task(void *, void *, void *)
{
	const struct device *baro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_baro));
	const int baro_hz = CONFIG_BARO_FREQUENCY_VALUE;

	if (baro_init(baro0)) {
		LOG_ERR("Baro not ready!");
		return;
	}
	baro_active = true;

	struct sensor_value temp, press;

	// int64_t lasttime = k_uptime_get();

	while (1) {

		if (baro_measure(baro0, &temp, &press)) {
			LOG_ERR("Failed to measure baro0");
			continue;
		}

		// currently only uses baro0 for altitude measurement
		altitude = baro_altitude(sensor_value_to_float(&press));

		K_SPINLOCK(&sensor_lock)
		{
			sensor_update = true;
		}

		LOG_INF("[baro0] Temperature: %d.%06d | Pressure: %d.%06d | Altitude: %.2f\n",
				temp.val1, temp.val2, press.val1, press.val2, (double)altitude);

		// int64_t delta = k_uptime_delta(&lasttime);
		/* print the hz */
		// LOG_DBG("BARO Hz: %.2f", 1000.0 / delta);

		k_sleep(K_MSEC(1000 / baro_hz));
	}
}

/* Create the BARO task */
K_THREAD_DEFINE(baro_task_id, 2048, baro_task, NULL, NULL, NULL,
				5, 0, 0);
#endif /* CONFIG_BARO */
#endif /* CONFIG_AURORA_SENSORS */

/* ============================================================
 *                     State machine TASK
 * ============================================================ */
#if defined(CONFIG_AURORA_STATE_MACHINE)
void state_machine_task(void *, void *, void *)
{

	k_spinlock_key_t key;
	struct sm_inputs inputs = (struct sm_inputs){
		.armed = 1,
		.orientation = orientation,
		.acceleration = acceleration,
		.velocity = velocity,
		.altitude = altitude,
	};

	sm_init(&state_cfg, NULL);
	sm_active = true;

	// TODO: Add idling
	while(!baro_active && !imu_active);

	while (1) {

		// trylock to check if there is a new sensor update, if not just skip the state machine update and sleep
		if (k_spin_trylock(&sensor_lock, &key) == 0) {
			if (sensor_update) {
				inputs = (struct sm_inputs){
					.armed = 1,
					.orientation = orientation,
					.acceleration = acceleration,
					.velocity = velocity,
					.altitude = altitude,
				};

				sensor_update = false;
				sm_update(&inputs);
				LOG_INF("STATE = %d\n", sm_get_state());
			}
			k_spin_unlock(&sensor_lock, key);
		}

		/* currently 10Hz. TODO: JUST FOR TESTING! */
		k_sleep(K_MSEC(1));
	}
}

/* Create the State machine task */
K_THREAD_DEFINE(state_machine_task_id, 2048, state_machine_task, NULL, NULL,
				NULL, 5, 0, 0);
#endif /* CONFIG_AURORA_STATE_MACHINE */

/* ============================================================
 *                     MAIN INITIALIZATION
 * ============================================================ */
int main(void)
{
	LOG_INF("Auxspace AURORA %s", APP_VERSION_STRING);

	/* Threads start automatically via K_THREAD_DEFINE */

	return 0;
}
