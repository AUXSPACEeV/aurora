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

#if defined(CONFIG_DATA_LOGGER)
#include <aurora/lib/data_logger.h>
#endif /* CONFIG_DATA_LOGGER */

#if defined(CONFIG_AURORA_NOTIFY)
#include <aurora/lib/notify.h>
#endif /* CONFIG_AURORA_NOTIFY */

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

#if defined(CONFIG_DATA_LOGGER) && defined(CONFIG_IMU) && defined(CONFIG_BARO)
/** Flush the data logger every N SM update cycles via the system workqueue. */
#define DATA_LOGGER_FLUSH_INTERVAL 10

static struct data_logger sm_logger;
static struct k_work flush_work;

static void flush_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	data_logger_flush(&sm_logger);
}
#endif /* CONFIG_DATA_LOGGER */

static bool baro_active = false; /**< True once the barometer thread has initialized. */
static bool imu_active = false;  /**< True once the IMU thread has initialized. */
static bool sm_active = false;   /**< True once the state machine thread has initialized. */

/* ============================================================
 *                     IMU TASK
 * ============================================================ */
#if defined(CONFIG_IMU)
/**
 * @brief IMU polling thread.
 *
 * Initializes the IMU and continuously polls orientation and acceleration
 * at the configured frequency, updating the global sensor variables.
 */
void imu_task(void *, void *, void *)
{
	const struct device *imu0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_imu));

	if (imu_init(imu0)) {
		LOG_ERR("IMU not ready!");
		return;
	}
	imu_active = true;

#if !defined(CONFIG_IMU_TRIGGER)
	const int imu_hz = CONFIG_IMU_FREQUENCY_VALUE;
	while (1) {
		int rc = imu_poll(imu0);
		if (rc != 0) {
			LOG_ERR("IMU polling failed (%d)", rc);
			continue;
		}
		k_sleep(K_MSEC(1000 / imu_hz));
	}
#endif /* !CONFIG_IMU_TRIGGER */

	LOG_INF("IMU task ended.");
}

/* Create the IMU task (inactive unless CONFIG_IMU=y) */
K_THREAD_DEFINE(imu_task_id, 2048, imu_task, NULL, NULL, NULL,
				5, 0, 0);
#endif /* CONFIG_IMU */

/* ============================================================
 *                     BARO TASK
 * ============================================================ */
#if defined(CONFIG_BARO)
/**
 * @brief Barometer polling thread.
 *
 * Initializes the barometric sensor and continuously measures temperature,
 * pressure at the configured frequency.
 */
void baro_task(void *, void *, void *)
{
	const struct device *baro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_baro));

	if (baro_init(baro0)) {
		LOG_ERR("Baro not ready!");
		return;
	}
	baro_active = true;

#if !defined(CONFIG_BARO_TRIGGER)
	const int baro_hz = CONFIG_BARO_FREQUENCY_VALUE;
	while (1) {
		if (baro_measure(baro0)) {
			LOG_ERR("Failed to measure baro0");
			continue;
		}

		k_sleep(K_MSEC(1000 / baro_hz));
	}
#endif
}

/* Create the BARO task */
K_THREAD_DEFINE(baro_task_id, 2048, baro_task, NULL, NULL, NULL,
				5, 0, 0);
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
#if defined(CONFIG_AURORA_NOTIFY)
	enum sm_state prev_state = SM_IDLE;
#endif /* CONFIG_AURORA_NOTIFY */
	const struct zbus_channel *data_chan;
	union {
		struct imu_data imu;
		struct baro_data baro;
	} msg_buf;
	double altitude = 0.0;
	double acceleration = 0.0;
	double orientation = 0.0;
	bool baro_ready = false;
	bool imu_ready = false;
#if defined(CONFIG_DATA_LOGGER) && defined(CONFIG_IMU) && defined(CONFIG_BARO)
	struct imu_data last_imu = {0};
	struct baro_data last_baro = {0};
	uint32_t log_count = 0;
#endif /* CONFIG_DATA_LOGGER */

#if defined(CONFIG_PYRO)
	const struct device *pyro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_pyro));
	enum sm_state pyro_state = SM_IDLE;
	int ret;
#endif /* CONFIG_PYRO */

	struct sm_inputs inputs = (struct sm_inputs){
		.armed = 1,
		.orientation = orientation,
		.acceleration = acceleration,
	};

#if defined(CONFIG_PYRO)
	while (!device_is_ready(pyro0)) {
		LOG_ERR("Pyro device %s is not ready, trying again ...",
				pyro0->name);
		k_sleep(K_SECONDS(1));
	}
#endif /*.CONFIG_PYRO */

	sm_init(&state_cfg, NULL);
	sm_active = true;

#if defined(CONFIG_DATA_LOGGER) && defined(CONFIG_IMU) && defined(CONFIG_BARO)
	k_work_init(&flush_work, flush_work_handler);
	if (data_logger_init(&sm_logger, "flight") != 0) {
		LOG_ERR("data_logger_init failed");
	}
#endif /* CONFIG_DATA_LOGGER */

	// TODO: Add idling
	while (!baro_active || !imu_active) {
		k_sleep(K_MSEC(100));
	}

	while (1) {
		/* Block until at least one message arrives */
		if (zbus_sub_wait_msg(&sm_sub, &data_chan, &msg_buf, K_FOREVER) != 0) {
			continue;
		}

		/* Process the first message, then drain any queued messages
		 * so we always work with the latest sensor data.
		 */
		do {
			if (data_chan == &imu_data_chan) {
				if (imu_sensor_value_to_orientation(&msg_buf.imu, &orientation) == 0 &&
				    imu_sensor_value_to_acceleration(&msg_buf.imu, &acceleration) == 0) {
					imu_ready = true;
#if defined(CONFIG_DATA_LOGGER) && defined(CONFIG_IMU) && defined(CONFIG_BARO)
					last_imu = msg_buf.imu;
#endif /* CONFIG_DATA_LOGGER */
				}
			} else if (data_chan == &baro_data_chan) {
				if (baro_sensor_value_to_altitude(&msg_buf.baro.pressure, &altitude) == 0) {
					baro_ready = true;
#if defined(CONFIG_DATA_LOGGER) && defined(CONFIG_IMU) && defined(CONFIG_BARO)
					last_baro = msg_buf.baro;
#endif /* CONFIG_DATA_LOGGER */
				}
			}
		} while (zbus_sub_wait_msg(&sm_sub, &data_chan, &msg_buf, K_NO_WAIT) == 0);

		/* Only update state machine once both sensors have fresh data */
		if (!baro_ready || !imu_ready) {
			continue;
		}

#if defined(CONFIG_DATA_LOGGER) && defined(CONFIG_IMU) && defined(CONFIG_BARO)
		{
			int64_t ts = k_uptime_get();
			struct datapoint dp = {
				.timestamp_ms = ts,
				.type = AURORA_DATA_IMU_ACCEL,
				.channel_count = 3,
				.channels = { last_imu.accel[0],
					      last_imu.accel[1],
					      last_imu.accel[2] },
			};
			data_logger_log(&sm_logger, &dp);

			dp.type = AURORA_DATA_IMU_GYRO;
			dp.channels[0] = last_imu.gyro[0];
			dp.channels[1] = last_imu.gyro[1];
			dp.channels[2] = last_imu.gyro[2];
			data_logger_log(&sm_logger, &dp);

			dp.type = AURORA_DATA_BARO;
			dp.channel_count = 2;
			dp.channels[0] = last_baro.temperature;
			dp.channels[1] = last_baro.pressure;
			data_logger_log(&sm_logger, &dp);

			if (++log_count >= DATA_LOGGER_FLUSH_INTERVAL) {
				k_work_submit(&flush_work);
				log_count = 0;
			}
		}
#endif /* CONFIG_DATA_LOGGER */

		inputs = (struct sm_inputs){
			.orientation = orientation,
			.acceleration = acceleration,
			.altitude = altitude,
		};

		sm_update(&inputs);
		state = sm_get_state();
		LOG_DBG("STATE = %d", state);

#if defined(CONFIG_AURORA_NOTIFY)
		if (state != prev_state) {
			notify_state_change(prev_state, state);
			prev_state = state;
		}
#endif /* CONFIG_AURORA_NOTIFY */

		/* reset the measurements */
		baro_ready = false;
		imu_ready = false;

#if defined(CONFIG_PYRO)
		if (state != pyro_state) {
			switch (state) {
			case SM_ARMED:
				ret = pyro_arm(pyro0, 0);
				if (ret)
					LOG_ERR("Failed to arm pyro module.");
				break;
			case SM_MAIN:
				ret = pyro_trigger_channel(pyro0, 0);
				if (ret)
					LOG_ERR("Failed to trigger pyro channel 0.");
				break;
			case SM_REDUNDAND:
				ret = pyro_trigger_channel(pyro0, 1);
				if (ret)
					LOG_ERR("Failed to trigger pyro channel 1.");
				break;
			default:
				break;
			}
			pyro_state = state;
		}
#endif /* CONFIG_PYRO */

	}
}

/* Create the State machine task */
K_THREAD_DEFINE(state_machine_task_id, 4096, state_machine_task, NULL, NULL,
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

#if defined(CONFIG_AURORA_NOTIFY)
	notify_init();
	notify_boot();
#endif /* CONFIG_AURORA_NOTIFY */

	/* Threads start automatically via K_THREAD_DEFINE */

	return 0;
}
