/**
 * @file main.c
 * @brief Sensor board application entry point.
 *
 * Defines three Zephyr threads (IMU, barometer, state machine) that run
 * concurrently to collect sensor data and drive the flight state machine.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aurora/lib/state/simple.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/zbus/zbus.h>

#include <app_version.h>

#if defined(CONFIG_IMU)
#include <aurora/lib/imu.h>
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#include <aurora/lib/baro.h>
#endif /* CONFIG_BARO */

#if defined(CONFIG_PYRO)
#include <aurora/drivers/pyro.h>
#endif /* CONFIG_PYRO */

#if defined(CONFIG_AURORA_STATE_MACHINE)
#include <aurora/lib/state/state.h>

/** @brief Flight state machine thresholds loaded from Kconfig. */
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

ZBUS_MSG_SUBSCRIBER_DEFINE(sm_sub);
ZBUS_CHAN_ADD_OBS(imu_data_chan, sm_sub, 1);
ZBUS_CHAN_ADD_OBS(baro_data_chan, sm_sub, 1);

static bool baro_active = false; /**< True once the barometer thread has initialized. */
static bool imu_active = false;  /**< True once the IMU thread has initialized. */
static bool sm_active = false;   /**< True once the state machine thread has initialized. */

/* ============================================================
 *                     IMU TASK
 * ============================================================ */
#if defined(CONFIG_IMU) && !defined(CONFIG_LSM6DSO_TRIGGER)
/**
 * @brief IMU polling thread.
 *
 * Initializes the IMU and continuously polls orientation and acceleration
 * at the configured frequency, updating the global sensor variables.
 */
void imu_task(void *, void *, void *)
{
	const struct device *imu0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_imu));
	const int imu_hz = CONFIG_IMU_FREQUENCY_VALUE;

	imu_init(imu0);
	imu_active = true;

	while (1) {
		int rc = imu_poll(imu0);
		if (rc != 0) {
			LOG_ERR("IMU polling failed (%d)", rc);
			continue;
		}
		k_sleep(K_MSEC(1000 / imu_hz));
	}

	LOG_INF("IMU task stopped.");
}

/* Create the IMU task (inactive unless CONFIG_IMU=y) */
K_THREAD_DEFINE(imu_task_id, 2048, imu_task, NULL, NULL, NULL,
				5, 0, 0);
#elif defined(CONFIG_IMU) && defined(CONFIG_LSM6DSO_TRIGGER)
	static const struct device *imu0 = NULL;
#endif /* CONFIG_IMU */

/* ============================================================
 *                     BARO TASK
 * ============================================================ */
#if defined(CONFIG_BARO) && !defined(CONFIG_LPS22HH_TRIGGER)
/**
 * @brief Barometer polling thread.
 *
 * Initializes the barometric sensor and continuously measures temperature,
 * pressure at the configured frequency.
 */
void baro_task(void *, void *, void *)
{
	const struct device *baro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_baro));
	const int baro_hz = CONFIG_BARO_FREQUENCY_VALUE;

	if (baro_init(baro0)) {
		LOG_ERR("Baro not ready!");
		return;
	}
	baro_active = true;

	while (1) {

		if (baro_measure(baro0)) {
			LOG_ERR("Failed to measure baro0");
			continue;
		}

		k_sleep(K_MSEC(1000 / baro_hz));
	}
}

/* Create the BARO task */
K_THREAD_DEFINE(baro_task_id, 2048, baro_task, NULL, NULL, NULL,
				5, 0, 0);
#elif defined(CONFIG_BARO) && defined(CONFIG_LPS22HH_TRIGGER)
	static const struct device *baro0 = NULL;
#endif /* CONFIG_BARO */

/* ============================================================
 *                     State machine TASK
 * ============================================================ */
#if defined(CONFIG_AURORA_STATE_MACHINE)
/**
 * @brief State machine thread.
 *
 * Waits for IMU and barometer readiness, then runs the flight state machine
 * at 10 Hz. Fires pyro channels on the appropriate state transitions.
 */
void state_machine_task(void *, void *, void *)
{
	enum sm_state state;
	const struct zbus_channel *data_chan;
	union {
		imu_data_t imu;
		baro_data_t baro;
	} msg_buf;
	float altitude = 0.0f;
	float acceleration = 0.0f;
	float orientation = 0.0f;
	float velocity = 0.0f;
	bool baro_ready = false;
	bool imu_ready = false;

#if defined(CONFIG_PYRO)
	const struct device *pyro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_pyro));
	int ret;
#endif /* CONFIG_PYRO */

	struct sm_inputs inputs = (struct sm_inputs){
		.armed = 1,
		.orientation = orientation,
		.acceleration = acceleration,
		.velocity = velocity,
	};

#if defined(CONFIG_PYRO)
	while (!device_is_ready(pyro0)) {
		LOG_ERR("Pyro device %s is not ready, trying again ...\n",
				pyro0->name);
		k_sleep(K_SECONDS(1));
	}
#endif /*.CONFIG_PYRO */

	sm_init(&state_cfg, NULL);
	sm_active = true;

	// TODO: Add idling
	while (!baro_active || !imu_active) {
		k_sleep(K_MSEC(100));
	}

	/* subscribe to the channels of baro and imu*/


	while (1) {
		/* Check for new IMU or baro data and update inputs */
		if (zbus_sub_wait_msg(&sm_sub, &data_chan, &msg_buf, K_FOREVER) == 0)
		{
			if (data_chan == &imu_data_chan) {
				if (imu_sensor_value_to_orientation(&msg_buf.imu, &orientation) == 0 &&
				    imu_sensor_value_to_acceleration(&msg_buf.imu, &acceleration) == 0) {
					LOG_INF("orientation: %f deg. acc: %f\n", (double)orientation,
							(double)acceleration);
					imu_ready = true;
				} else {
					LOG_ERR("IMU conversion failed");
				}
			} else if (data_chan == &baro_data_chan) {
				if (baro_sensor_value_to_altitude(&msg_buf.baro.pressure, &altitude) == 0) {
					LOG_INF("Baro: pressure=%.2f kPa, altitude=%d.%02d m\n",
							(double)msg_buf.baro.pressure.val1 + (double)msg_buf.baro.pressure.val2 / 1e6,
							(int)altitude, abs((int)(altitude * 100) % 100));
					baro_ready = true;
				} else {
					LOG_ERR("Baro conversion failed");
				}
			}
		}

		/* only update state machine if both sensors values where updated*/
		if (!baro_ready || !imu_ready) {
			continue;
		}

		inputs = (struct sm_inputs){
			.orientation = orientation,
			.acceleration = acceleration,
			.velocity = velocity,
			.altitude = altitude,
		};

		sm_update(&inputs);
		state = sm_get_state();
		LOG_INF("STATE = %d\n", state);

		/* reset the measurements */
		baro_ready = false;
		imu_ready = false;

#if defined(CONFIG_PYRO)
		switch (state) {
		case SM_IDLE:
			break;
		case SM_ARMED:
			ret = pyro_arm(pyro0, 0);
			if (ret)
				LOG_ERR("Failed to arm pyro module.\n");
			break;
		case SM_MAIN:
			ret = pyro_trigger_channel(pyro0, 0);
			if (ret)
				LOG_ERR("Failed to trigger pyro channel 0.\n");
			break;
		case SM_REDUNDAND:
			ret = pyro_trigger_channel(pyro0, 1);
			if (ret)
				LOG_ERR("Failed to trigger pyro channel 1.\n");
			break;
		default:
			break;
		}
#endif /* CONFIG_PYRO */

	}
}

/* Create the State machine task */
K_THREAD_DEFINE(state_machine_task_id, 2048, state_machine_task, NULL, NULL,
				NULL, 5, 0, 0);
#endif /* CONFIG_AURORA_STATE_MACHINE */

/* ============================================================
 *                     MAIN INITIALIZATION
 * ============================================================ */
/**
 * @brief Application entry point.
 *
 * Logs the firmware version. All work is performed by threads started
 * automatically via K_THREAD_DEFINE.
 */
int main(void)
{
	LOG_INF("Auxspace AURORA %s", APP_VERSION_STRING);


	#if defined(CONFIG_IMU) && defined(CONFIG_LSM6DSO_TRIGGER)
	imu0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_imu));
	if (imu_init(imu0) != 0) {
		LOG_ERR("IMU device %s failed to initialize", imu0->name);
		return -1;
	}

	imu_active = true;
	#endif /* CONFIG_IMU */

	#if defined(CONFIG_BARO) && defined(CONFIG_LPS22HH_TRIGGER)
	baro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_baro));
	if (baro_init(baro0) != 0) {
		LOG_ERR("Baro device %s failed to initialize", baro0->name);
		return -1;
	}
	baro_active = true;
	#endif /* CONFIG_BARO */

	/* Threads start automatically via K_THREAD_DEFINE */

	return 0;
}
