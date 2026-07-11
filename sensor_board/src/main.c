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

#include "data.h"

#include <string.h>
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
/* Build-time dependency: the state machine fires a pyro device selected via the
 * 'auxspace,pyro' chosen node. */
#if !DT_HAS_CHOSEN(auxspace_pyro)
#error "CONFIG_PYRO requires DT chosen 'auxspace,pyro' to point at a pyro device node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_pyro), okay),
	     "the 'auxspace,pyro' chosen node must have status \"okay\"");
#endif /* CONFIG_PYRO */

#if defined(CONFIG_DATA_LOGGER_BIN)
#include <aurora/lib/data_logger.h>
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

LOG_MODULE_REGISTER(main, CONFIG_SENSOR_BOARD_LOG_LEVEL);

ZBUS_MSG_SUBSCRIBER_DEFINE(sm_sub);

#if defined (CONFIG_IMU)
ZBUS_CHAN_ADD_OBS(imu_data_chan, sm_sub, 1);
#endif

#if defined (CONFIG_BARO)
ZBUS_CHAN_ADD_OBS(baro_data_chan, sm_sub, 1);
#endif

#if defined(CONFIG_AURORA_POWERFAIL)
static void powerfail_assert(void)
{
#if defined(CONFIG_AURORA_STATE_MACHINE)
	armed = 0;
#endif /* CONFIG_AURORA_STATE_MACHINE */
}

static void powerfail_deassert(void)
{
#if defined(CONFIG_AURORA_STATE_MACHINE)
	armed = 1;
#endif /* CONFIG_AURORA_STATE_MACHINE */
}
#endif /* CONFIG_AURORA_POWERFAIL */

bool baro_active = false; /**< True once the barometer thread has initialized. */
bool imu_active = false;  /**< True once the IMU thread has initialized. */
static bool sm_active = false; /**< True once the state machine thread has initialized. */

/* ============================================================
 *                     IMU TASK
 * ============================================================ */
#if defined(CONFIG_IMU) && !defined(CONFIG_AURORA_FAKE_SENSORS)
/* Build-time dependency: the IMU task fetches its device from the
 * 'auxspace,imu' chosen node. */
#if !DT_HAS_CHOSEN(auxspace_imu)
#error "CONFIG_IMU requires DT chosen 'auxspace,imu' to point at an IMU sensor node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_imu), okay),
	     "the 'auxspace,imu' chosen node must have status \"okay\"");
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
	/* IDLE/ARMED may abort to ERROR when the flight log is offline. */
	case SM_IDLE: return (to == SM_ARMED || to == SM_ERROR);
	case SM_ARMED: return (to == SM_BOOST || to == SM_ERROR);
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

/* Re-signal interval while the state machine is held in SM_ERROR: the
 * buzzer replays the error melody and the log line repeats, so the failure
 * stays audible in the field without flooding the queue or the console.
 */
#define SM_ERROR_RESIGNAL_MS 5000

/**
 * @brief Error handler for the state machine.
 *
 * Pre-flight interlocks (flight log offline) hold SM_ERROR so the LEDs
 * stay in the error pattern and the buzzer re-beeps periodically. An
 * unmissable field indication with no console attached.  The operator
 * acknowledges by disarming, which forces the machine back to IDLE.
 * In-flight aborts (apogee/redundant timeout) keep the existing recover-
 * to-IDLE behavior so recovery hardware safes itself.
 *
 * @param reason why the state machine entered SM_ERROR
 * @param args arguments passed from the state machine (unused here)
 * @retval 0 to return to IDLE, negative errno to hold SM_ERROR
 */
int state_machine_error_handler(enum sm_error_reason reason, void *args)
{
	ARG_UNUSED(args);
#if defined(CONFIG_AURORA_FAKE_SENSORS)
	__ASSERT(false, "State machine ERROR (%s) in simulation",
		 sm_error_reason_str(reason));
#endif /* CONFIG_AURORA_FAKE_SENSORS */

	if (reason == SM_ERR_LOG_OFFLINE) {
		static int64_t last_signal = -SM_ERROR_RESIGNAL_MS;
		int64_t now = k_uptime_get();

		if ((now - last_signal) >= SM_ERROR_RESIGNAL_MS) {
			LOG_ERR("Arming refused: flight log offline. "
				"Disarm to acknowledge, reboot to retry");
#if defined(CONFIG_AURORA_NOTIFY)
			(void)notify_error();
#endif /* CONFIG_AURORA_NOTIFY */
			last_signal = now;
		}
		return -ENODEV;
	}

	LOG_ERR("State machine error: %s. Returning to IDLE",
		sm_error_reason_str(reason));
#if defined(CONFIG_AURORA_NOTIFY)
	(void)notify_error();
#endif /* CONFIG_AURORA_NOTIFY */
	return 0;
}

/**
 * @brief Processes raw IMU data to update orientation, acceleration, and attitude filtering.
 *
 * Calculates delta-time since the last sample, converts raw sensor data to double precision,
 * and updates the attitude estimation filter. Handles sensor calibration tracking while the
 * system is in the ARMED state, and computes vertical acceleration once calibrated.
 *
 * @param[in,out] last_imu_ns         Pointer to the timestamp of the last processed IMU sample.
 * @param[in,out] attitude_state      Pointer to the internal attitude estimation filter state.
 * @param[in]     imu_data            Pointer to the incoming raw IMU message buffer.
 * @param[out]    orientation         Array where the calculated orientation [pitch, roll, yaw] is stored.
 * @param[out]    acceleration        Pointer where the scalar acceleration magnitude is stored.
 * @param[out]    accel_vert          Pointer where the earth-frame vertical acceleration is stored.
 * @param[out]    imu_ready           Pointer set to true once fresh orientation and acceleration are available.
 * @param[in,out] calibration_notified Pointer tracking whether the user has been notified of completion.
 */
static void handle_imu(int64_t *last_imu_ns, struct attitude *attitude_state, struct imu_data *imu_data,
	double orientation[3], double *acceleration, double *accel_vert, bool *imu_ready, bool *calibration_notified)
{
	int64_t now_ns = (k_uptime_ticks() * NSEC_PER_SEC) / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	/* Delta-time since the last sample, clamped to a sane range; 0.0 on the
	 * first sample or after a discontinuity. Shared by orientation
	 * integration and the attitude update below.
	 */
	double dt_s = (*last_imu_ns != 0) ? (double)(now_ns - *last_imu_ns) / 1e9 : 0.0;
	if (dt_s < 0.0 || dt_s > 1.0) {
		dt_s = 0.0;
	}

	const double *bias_for_orient = attitude_is_calibrated(attitude_state) ? attitude_state->gyro_bias : NULL;

	if (imu_sensor_value_to_orientation(imu_data, dt_s, bias_for_orient, orientation) == 0
		&& imu_sensor_value_to_acceleration(imu_data, acceleration) == 0) {
		*imu_ready = true;
	}

	double accel_b[ATTITUDE_NUM_AXES] = {
		sensor_value_to_double(&imu_data->accel[0]),
		sensor_value_to_double(&imu_data->accel[1]),
		sensor_value_to_double(&imu_data->accel[2]),
	};
	double gyro_b[ATTITUDE_NUM_AXES] = {
		sensor_value_to_double(&imu_data->gyro[0]),
		sensor_value_to_double(&imu_data->gyro[1]),
		sensor_value_to_double(&imu_data->gyro[2]),
	};

	if (!attitude_is_calibrated(attitude_state)) {
		if (sm_get_state() == SM_ARMED) {
			attitude_calibrate_sample(attitude_state, accel_b, gyro_b);
			if (attitude_state->cal_samples >= CONFIG_IMU_CALIBRATION_SAMPLES) {
				if (attitude_calibrate_finish(attitude_state) == 0) {
#if defined(CONFIG_AURORA_NOTIFY)
					if (!(*calibration_notified)) {
						notify_calibration_complete();
						*calibration_notified = true;
					}
#endif /* CONFIG_AURORA_NOTIFY */
#if defined(CONFIG_AURORA_FAKE_SENSORS)
					fake_sensors_on_calibrated();
#endif /* CONFIG_AURORA_FAKE_SENSORS */
				}
			}
		}
		*accel_vert = 0.0;
	} else if (dt_s > 0.0) {
		double a_v;
		if (attitude_update(attitude_state, accel_b, gyro_b, dt_s, &a_v) == 0) {
			*accel_vert = a_v;
		}
	}
	*last_imu_ns = now_ns;
}

/**
 * @brief Handles pyrotechnic channel actions based on flight state transitions.
 *
 * Checks if a state change occurred, and triggers, arms, charges, or disarms
 * the pyro channels accordingly. Tracks the internal pyro subsystem state
 * to avoid redundant hardware calls.
 *
 * @param[in]     state      The current state of the flight state machine.
 * @param[in,out] pyro_state Pointer to the previously processed pyro state.
 * @param[in]     pyro0      Pointer to the Zephyr device structure for the pyro hardware.
 */
static void handle_pyro(enum sm_state state, enum sm_state *pyro_state, const struct device *pyro0)
{
#if defined(CONFIG_PYRO)
	if (state == *pyro_state)
		return;

#define PYRO_ACT(fn, ch, past, action)                                                                                 \
	do {                                                                                                               \
		if (fn(pyro0, ch))                                                                                             \
			LOG_ERR("Failed to " action " pyro0 channel " #ch);                                                        \
		else                                                                                                           \
			LOG_INF(past " pyro0 channel " #ch);                                                                       \
	} while (0)

	switch (state) {
	case SM_IDLE:
	/* SM_ERROR is only ever held pre-flight (arming interlock); keep the
	 * channels safe while the operator sorts out the error condition.
	 */
	case SM_ERROR:
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
	*pyro_state = state;
#endif /* CONFIG_PYRO */
}

static void handle_state_transition(enum sm_state prev_state, enum sm_state state, struct attitude *attitude_state,
	int64_t *last_imu_ns, bool *calibration_notified, double orientation[3])
{
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
		attitude_init(attitude_state);
		*last_imu_ns = 0;
		*calibration_notified = false;
		orientation[2] = 0.0;
	}
#endif /* CONFIG_IMU */
	log_handle_flight_lifecycle(prev_state, state);
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

	struct sm_inputs inputs = {
		.armed = armed,
		.acceleration = acceleration,
		.accel_vert = accel_vert,
	};
	memcpy(inputs.orientation, orientation, sizeof(inputs.orientation));
#if defined(CONFIG_IMU)
	static struct attitude attitude_state;
	int64_t last_imu_ns = 0;
	bool calibration_notified = false;

	attitude_init(&attitude_state);
#endif /* CONFIG_IMU */

#if defined(CONFIG_PYRO)
	const struct device *pyro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_pyro));
	enum sm_state pyro_state = SM_IDLE;

	while (!device_is_ready(pyro0)) {
		LOG_ERR("Pyro device %s is not ready, trying again ...", pyro0->name);
		k_sleep(K_SECONDS(1));
	}
#else
	const struct device *pyro0 = NULL;
	enum sm_state pyro_state = SM_IDLE;
#endif /*.CONFIG_PYRO */

	sm_init(&state_cfg, &sm_error_handler);
	sm_active = true;

	/* TODO: Add idling */
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
				handle_imu(&last_imu_ns,
					&attitude_state,
					&msg_buf.imu,
					orientation,
					&acceleration,
					&accel_vert,
					&imu_ready,
					&calibration_notified);
				log_imu_data(&msg_buf.imu);
#endif
#if defined(CONFIG_BARO)
			} else if (data_chan == &baro_data_chan) {
				log_baro_data(&msg_buf.baro);

				if (baro_sensor_value_to_altitude(&msg_buf.baro.pressure, &altitude) == 0) {
					baro_ready = true;
				}
#endif
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
			.log_ready = log_flight_log_online(),
			.acceleration = acceleration,
			.accel_vert = accel_vert,
			.altitude = altitude,
		};
		memcpy(inputs.orientation, orientation, sizeof(inputs.orientation));

		sm_update(&inputs);
		state = sm_get_state();
		LOG_DBG("STATE = %d", state);

		/*update pad link data*/
		update_pad_link_data();

		log_flight_telemetry();
		log_vbat_telemetry();

		if (state != prev_state) {
#if defined(CONFIG_IMU)
			handle_state_transition(prev_state,
				state,
				&attitude_state,
				&last_imu_ns,
				&calibration_notified,
				orientation);
#else
			handle_state_transition(prev_state, state, NULL, NULL, NULL, orientation);
#endif
			prev_state = state;
		}

		/* reset the measurements */
		baro_ready = false;
		imu_ready = false;

		handle_pyro(state, &pyro_state, pyro0);
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
