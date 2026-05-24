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

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <app_version.h>

#if defined(CONFIG_IMU)
#include <aurora/lib/attitude.h>
#include <aurora/lib/imu.h>
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#include <aurora/lib/baro.h>
#endif /* CONFIG_BARO */

#if defined(CONFIG_PYRO)
#include <aurora/drivers/pyro.h>
#endif /* CONFIG_PYRO */

#if defined(CONFIG_DATA_LOGGER_BIN)
#include <aurora/lib/data_logger.h>
#include <zephyr/fs/fs.h>
LOG_MODULE_REGISTER(main, CONFIG_SENSOR_BOARD_LOG_LEVEL);
#include "logger.h"
#endif /* CONFIG_DATA_LOGGER_BIN */

#if defined(CONFIG_AURORA_NOTIFY)
#include <aurora/lib/notify.h>
#endif /* CONFIG_AURORA_NOTIFY */

#if defined(CONFIG_AURORA_POWERFAIL)
#include <aurora/lib/powerfail.h>
#endif /* CONFIG_AURORA_POWERFAIL */

#if defined(CONFIG_AURORA_TELEMETRY)
#include <aurora/lib/telemetry.h>
#endif /* CONFIG_AURORA_TELEMETRY */

#if defined(CONFIG_AURORA_PAD_LINK)
#include <aurora/lib/pad_link.h>
#endif /* CONFIG_AURORA_PAD_LINK */

#if defined(CONFIG_AURORA_STATE_MACHINE)
#include <aurora/lib/state/state.h>
static int armed = 0;

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
	.N_OI = CONFIG_DISARM_ANGLE_SAMPLES,
	/* Timers */
	.DT_AB = CONFIG_BOOST_TIMER_MS,
	.DT_L = CONFIG_LANDING_TIMER_MS,

	/* Timeouts */
	.TO_A = CONFIG_APOGEE_TIMEOUT_MS,
	.TO_M = CONFIG_MAIN_TIMEOUT_MS,
	.TO_R = CONFIG_REDUNDANT_TIMEOUT_MS,
};
#endif /* CONFIG_AURORA_STATE_MACHINE */

ZBUS_MSG_SUBSCRIBER_DEFINE(sm_sub);
ZBUS_CHAN_ADD_OBS(imu_data_chan, sm_sub, 1);
ZBUS_CHAN_ADD_OBS(baro_data_chan, sm_sub, 1);

#if defined(CONFIG_AURORA_POWERFAIL)
static void powerfail_assert()
{
#if defined(CONFIG_AURORA_STATE_MACHINE)
	armed = 0;
#endif /* CONFIG_AURORA_STATE_MACHINE */
}

static void powerfail_deassert()
{
#if defined(CONFIG_AURORA_STATE_MACHINE)
	armed = 1;
#endif /* CONFIG_AURORA_STATE_MACHINE */
}
#endif /* CONFIG_AURORA_POWERFAIL */

bool baro_active = false; /**< True once the barometer thread has initialized. */
bool imu_active = false;  /**< True once the IMU thread has initialized. */
static bool sm_active = false;	 /**< True once the state machine thread has initialized. */

/* ============================================================
 *                     IMU TASK
 * ============================================================ */
#if defined(CONFIG_IMU) && !defined(CONFIG_AURORA_FAKE_SENSORS)
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
	const int imu_hz = CONFIG_IMU_FREQUENCY;
	while (1) {
		int rc = imu_poll(imu0);
		if (rc != 0) {
			LOG_ERR("IMU polling failed (%d)", rc);
			continue;
		}
		k_sleep(K_MSEC(1000 / imu_hz));
	}
#endif /* !CONFIG_IMU_TRIGGER */

	LOG_INF("IMU ready");
}

/* Create the IMU task (inactive unless CONFIG_IMU=y) */
K_THREAD_DEFINE(imu_polling, 2048, imu_task, NULL, NULL, NULL, 7, 0, 0);
#endif /* CONFIG_IMU && !CONFIG_AURORA_FAKE_SENSORS */

/* ============================================================
 *                     BARO TASK
 * ============================================================ */
#if defined(CONFIG_BARO) && !defined(CONFIG_AURORA_FAKE_SENSORS)
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
	const int baro_hz = CONFIG_BARO_FREQUENCY;
	while (1) {
		if (baro_measure(baro0)) {
			LOG_ERR("Failed to measure baro0");
			continue;
		}

		k_sleep(K_MSEC(1000 / baro_hz));
	}
#endif /* !CONFIG_BARO_TRIGGER */

	LOG_INF("Baro ready");
}

/* Create the BARO task */
K_THREAD_DEFINE(baro_polling, 2048, baro_task, NULL, NULL, NULL, 7, 0, 0);
#endif /* CONFIG_BARO && !CONFIG_AURORA_FAKE_SENSORS */

/* ============================================================
 *                     State machine TASK
 * ============================================================ */
#if defined(CONFIG_AURORA_STATE_MACHINE)

#if defined(CONFIG_AURORA_FAKE_SENSORS)
/* make the function known (defined in fake_sensors.c) */
void fake_sensors_on_calibrated(void);
#endif /* CONFIG_AURORA_FAKE_SENSORS */

#if defined(CONFIG_AURORA_SIM_AUTOTEST)
/**
 * @brief checks if the state machine transition is valid in the context of the simulation
 *
 * @param from the previous state
 * @param to the new state
 * @retval true if the transition is valid
 * @retval false if the transition is invalid
 */
static bool is_valid_transition(enum sm_state from, enum sm_state to)
{
	/* Disarm brings any non-IDLE state back to IDLE */
	if (to == SM_IDLE) {
		return true;
	}

	switch (from) {
	case SM_IDLE: return (to == SM_ARMED);
	case SM_ARMED: return (to == SM_BOOST);
	case SM_BOOST: return (to == SM_BURNOUT);
	case SM_BURNOUT: return (to == SM_APOGEE);
	case SM_APOGEE: return (to == SM_MAIN || to == SM_ERROR);
	case SM_MAIN: return (to == SM_REDUNDANT);
	case SM_REDUNDANT: return (to == SM_LANDED || to == SM_ERROR);
	case SM_ERROR: return (to == SM_IDLE);
	case SM_LANDED: return false;
	default: return false;
	}
}
#endif /* CONFIG_AURORA_SIM_AUTOTEST */

/**
 * @brief Error handler for the state machine.
 *
 * @param args arguments passed from the state machine (unused here)
 * @retval 0 on completion
 */
int state_machine_error_handler(void *args)
{
	ARG_UNUSED(args);
#if defined(CONFIG_AURORA_FAKE_SENSORS)
	__ASSERT(false, "State machine reached ERROR in simulation");
#endif /* CONFIG_AURORA_FAKE_SENSORS */
	LOG_WRN("WTF is error handling? Just go back to IDLE");
	return 0;
}

/**
 * @brief State machine thread.
 *
 * Waits for IMU and barometer readiness, then runs the flight state machine
 * at 10 Hz. Fires pyro channels on the appropriate state transitions.
 */
void state_machine_task(void *, void *, void *)
{
	enum sm_state state;
	enum sm_state prev_state = SM_IDLE;
	const struct zbus_channel *data_chan;
	union {
		struct imu_data imu;
		struct baro_data baro;
	} msg_buf;
	double altitude = 0.0;
	double acceleration = 0.0;
	double accel_vert = 0.0;
	double orientation[] = {0.0, 0.0, 0.0};
	bool baro_ready = false;
	bool imu_ready = false;

	struct sm_error_handling_args sm_error_handler = {
		.cb = &state_machine_error_handler,
		.args = NULL,
	};

#if defined(CONFIG_IMU)
	static struct attitude attitude_state;
	int64_t last_imu_ns = 0;
	bool calibration_notified = false;

	attitude_init(&attitude_state);
#endif /* CONFIG_IMU */

#if defined(CONFIG_PYRO)
	const struct device *pyro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_pyro));
	enum sm_state pyro_state = SM_IDLE;
#endif /* CONFIG_PYRO */

	struct sm_inputs inputs = {
		.armed = armed,
		.acceleration = acceleration,
		.accel_vert = accel_vert,
	};
	memcpy(inputs.orientation, orientation, sizeof(inputs.orientation));

#if defined(CONFIG_PYRO)
	while (!device_is_ready(pyro0)) {
		LOG_ERR("Pyro device %s is not ready, trying again ...", pyro0->name);
		k_sleep(K_SECONDS(1));
	}
#endif /*.CONFIG_PYRO */

	sm_init(&state_cfg, &sm_error_handler);
	sm_active = true;

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
#if defined(CONFIG_IMU)
				int64_t now_ns = (k_uptime_ticks() * NSEC_PER_SEC) / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
				double dt_orient_s = (last_imu_ns != 0) ? (double)(now_ns - last_imu_ns) / 1e9 : 0.0;
				if (dt_orient_s < 0.0 || dt_orient_s > 1.0) {
					dt_orient_s = 0.0;
				}
				const double *bias_for_orient
					= attitude_is_calibrated(&attitude_state) ? attitude_state.gyro_bias : NULL;
#else
				const double dt_orient_s = 0.0;
				const double *bias_for_orient = NULL;
#endif /* CONFIG_IMU */
				if (imu_sensor_value_to_orientation(&msg_buf.imu, dt_orient_s, bias_for_orient, orientation) == 0
					&& imu_sensor_value_to_acceleration(&msg_buf.imu, &acceleration) == 0) {
					imu_ready = true;
				}

#if defined(CONFIG_IMU)
				{
					double accel_b[ATTITUDE_NUM_AXES] = {
						sensor_value_to_double(&msg_buf.imu.accel[0]),
						sensor_value_to_double(&msg_buf.imu.accel[1]),
						sensor_value_to_double(&msg_buf.imu.accel[2]),
					};
					double gyro_b[ATTITUDE_NUM_AXES] = {
						sensor_value_to_double(&msg_buf.imu.gyro[0]),
						sensor_value_to_double(&msg_buf.imu.gyro[1]),
						sensor_value_to_double(&msg_buf.imu.gyro[2]),
					};

					if (!attitude_is_calibrated(&attitude_state)) {
						if (sm_get_state() == SM_ARMED) {
							attitude_calibrate_sample(&attitude_state, accel_b, gyro_b);
							if (attitude_state.cal_samples >= CONFIG_IMU_CALIBRATION_SAMPLES) {
								if (attitude_calibrate_finish(&attitude_state) == 0) {
#if defined(CONFIG_AURORA_NOTIFY)
									if (!calibration_notified) {
										notify_calibration_complete();
										calibration_notified = true;
									}
#endif /* CONFIG_AURORA_NOTIFY */
#if defined(CONFIG_AURORA_FAKE_SENSORS)
									fake_sensors_on_calibrated();
#endif /* CONFIG_AURORA_FAKE_SENSORS */
								}
							}
						}
						accel_vert = 0.0;
					} else if (last_imu_ns != 0) {
						double dt_s = (double)(now_ns - last_imu_ns) / 1e9;
						if (dt_s > 0.0 && dt_s <= 1.0) {
							double a_v;
							if (attitude_update(&attitude_state, accel_b, gyro_b, dt_s, &a_v) == 0) {
								accel_vert = a_v;
							}
						}
					}
					last_imu_ns = now_ns;
				}
#endif /* CONFIG_IMU */
				log_imu_data(&msg_buf.imu);
			} else if (data_chan == &baro_data_chan) {
				log_baro_data(&msg_buf.baro);

				if (baro_sensor_value_to_altitude(&msg_buf.baro.pressure, &altitude) == 0) {
					baro_ready = true;
				}
			}
		} while (zbus_sub_wait_msg(&sm_sub, &data_chan, &msg_buf, K_NO_WAIT) == 0);

		/* This check is necessary to avoid updating the state machine
		 * with uninitialized sensor data durring startup. This ensures
		 * that updates to the state machine only after both the
		 * barometer and IMU have reported valid data at least once.
		 * The parameters don't have to be reset because the kalman filter
		 * used inside the state machine can handle updates to a single
		 * sensor (e.g. just the barometer. After receiving the first
		 * valid values from all sensors the ZBUS messages ensure
		 * that the state machine is only updated when at least
		 * one of the sensors has sent a new value.
		 */
		if (!baro_ready || !imu_ready) {
			continue;
		}

		inputs = (struct sm_inputs){
			.armed = armed,
			.acceleration = acceleration,
			.accel_vert = accel_vert,
			.altitude = altitude,
		};
		memcpy(inputs.orientation, orientation, sizeof(inputs.orientation));

		sm_update(&inputs);
		state = sm_get_state();
		LOG_DBG("STATE = %d", state);

		log_flight_telemetry();

		if (state != prev_state) {
#if defined(CONFIG_AURORA_FAKE_SENSORS)
			__ASSERT(is_valid_transition(prev_state, state),
				"Invalid SM transition: %s -> %s",
				sm_state_str(prev_state),
				sm_state_str(state));
#endif /* CONFIG_AURORA_FAKE_SENSORS */
#if defined(CONFIG_AURORA_NOTIFY)
			notify_state_change(prev_state, state);
#endif /* CONFIG_AURORA_NOTIFY */
#if defined(CONFIG_IMU)
			/* On return to IDLE, discard calibration so a re-arm
			 * triggers a fresh stationary calibration window.
			 */
			if (state == SM_IDLE) {
				attitude_init(&attitude_state);
				last_imu_ns = 0;
				calibration_notified = false;
				orientation[2] = 0.0;
			}
#endif /* CONFIG_IMU */
			log_handle_flight_lifecycle(prev_state, state);
			prev_state = state;
		}

#if defined(CONFIG_PYRO)
		if (state == pyro_state)
			continue;

#define PYRO_ACT(fn, ch, past, action)                                                                                 \
	do {                                                                                                               \
		if (fn(pyro0, ch))                                                                                             \
			LOG_ERR("Failed to " action " pyro0 channel " #ch);                                                        \
		else                                                                                                           \
			LOG_INF(past " pyro0 channel " #ch);                                                                       \
	} while (0)

		switch (state) {
		case SM_IDLE:
			PYRO_ACT(pyro_disarm, 0, "Disarmed", "disarm");
			PYRO_ACT(pyro_disarm, 1, "Disarmed", "disarm");
			break;
		case SM_ARMED:
			PYRO_ACT(pyro_arm, 0, "Armed", "arm");
			PYRO_ACT(pyro_arm, 1, "Armed", "arm");
			break;
		case SM_APOGEE:
			PYRO_ACT(pyro_trigger_channel, 0, "Triggered", "trigger");
			/* Capacitors are empty after trigger. Recharge! */
			PYRO_ACT(pyro_charge_channel, 1, "Charging", "charge");
			break;
		case SM_MAIN:
			PYRO_ACT(pyro_trigger_channel, 1, "Triggered", "trigger");
			/* Capacitors are empty after trigger. Recharge! */
			PYRO_ACT(pyro_charge_channel, 1, "Recharging", "recharge");
			break;
		case SM_REDUNDANT: PYRO_ACT(pyro_trigger_channel, 1, "Re-triggered", "re-trigger"); break;
		default: break;
		}
#undef PYRO_ACT
		pyro_state = state;
#endif /* CONFIG_PYRO */
	}
}

/* Create the State machine task */
K_THREAD_DEFINE(state_machine, 4096, state_machine_task, NULL, NULL, NULL, 6, 0, 0);
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

#if defined(CONFIG_AURORA_POWERFAIL)
	powerfail_setup(&powerfail_assert, &powerfail_deassert);
#else
	/* No powerfail module → assume always armed */
	armed = 1;
#endif /* CONFIG_AURORA_POWERFAIL */

#if defined(CONFIG_AURORA_NOTIFY)
	notify_init();
	notify_boot();
#endif /* CONFIG_AURORA_NOTIFY */

#if defined(CONFIG_AURORA_TELEMETRY)
	(void)telemetry_init();
#endif /* CONFIG_AURORA_TELEMETRY */

#if defined(CONFIG_AURORA_PAD_LINK)
	(void)pad_link_init();
#endif /* CONFIG_AURORA_PAD_LINK */

	return 0;
}
