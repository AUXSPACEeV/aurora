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
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/zbus/zbus.h>

#include <app_version.h>

#if defined(CONFIG_IMU)
#include <aurora/lib/imu.h>
#include <aurora/lib/attitude.h>
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

#if defined(CONFIG_AURORA_POWERFAIL)
#include <aurora/lib/powerfail.h>
#endif /* CONFIG_AURORA_POWERFAIL */

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
ZBUS_CHAN_ADD_OBS(imu_data_chan, sm_sub, 1);
ZBUS_CHAN_ADD_OBS(baro_data_chan, sm_sub, 1);

#if defined(CONFIG_DATA_LOGGER)
static struct data_logger sm_logger;
static atomic_t sm_logger_live = ATOMIC_INIT(0);

/* Decouple logging from the SM hot path: SM pushes datapoints into this
 * queue with K_NO_WAIT; a dedicated logger thread drains it and owns all
 * FS-touching operations (write + periodic flush). Sized to absorb the
 * worst-case flush stall at full IMU+baro rate.
 */
#define LOG_MSGQ_DEPTH     256
#define LOG_FLUSH_PERIOD_MS 1000

/* How long SM→ARMED will wait for a pending conversion to finish. */
#define LOG_ARM_CONVERT_TIMEOUT_MS 1500

K_MSGQ_DEFINE(log_msgq, sizeof(struct datapoint), LOG_MSGQ_DEPTH, 4);

/* convert_idle has one token whenever no conversion is in flight. The
 * converter thread holds it while running; SM→ARMED tries to acquire it
 * (with a short timeout) to verify the last flight has been fully
 * translated before opening a new binary file.
 */
K_SEM_DEFINE(convert_idle, 1, 1);

/* Woken by SM→(IDLE|LANDED|ERROR) to kick off conversion of the just-
 * closed binary file.
 */
K_SEM_DEFINE(convert_request, 0, 1);

/* Binary path captured at logger close, read by the converter thread.
 * Single-producer (SM) / single-consumer (converter); the request sem
 * acts as the handoff fence.
 */
static char convert_bin_path[DATA_LOGGER_PATH_MAX];

static inline void log_enqueue(const struct datapoint *dp)
{
	/* Drop on overflow rather than stall the SM thread. */
	(void)k_msgq_put(&log_msgq, dp, K_NO_WAIT);
}

static void logger_task(void *, void *, void *)
{
	struct datapoint dp;
	int64_t last_flush = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();
		int64_t wait_ms = LOG_FLUSH_PERIOD_MS - (now - last_flush);
		if (wait_ms < 0) {
			wait_ms = 0;
		}

		if (k_msgq_get(&log_msgq, &dp, K_MSEC(wait_ms)) == 0) {
			if (atomic_get(&sm_logger_live)) {
				data_logger_log(&dp);
			}
		}

		if (k_uptime_get() - last_flush >= LOG_FLUSH_PERIOD_MS) {
			if (atomic_get(&sm_logger_live)) {
				data_logger_flush(&sm_logger);
			}
			last_flush = k_uptime_get();
		}
	}
}

K_THREAD_DEFINE(logger_thread, 2048, logger_task, NULL, NULL, NULL,
		8, 0, 0);

static void swap_extension(char *dst, size_t dst_sz, const char *src,
			   const char *new_ext)
{
	strncpy(dst, src, dst_sz - 1);
	dst[dst_sz - 1] = '\0';

	char *dot = strrchr(dst, '.');
	char *slash = strrchr(dst, '/');

	if (dot != NULL && (slash == NULL || dot > slash)) {
		*dot = '\0';
	}

	size_t len = strlen(dst);

	if (len + 1 + strlen(new_ext) + 1 > dst_sz) {
		return;
	}
	dst[len] = '.';
	strcpy(dst + len + 1, new_ext);
}

static void converter_task(void *, void *, void *)
{
	while (1) {
		k_sem_take(&convert_request, K_FOREVER);
		k_sem_take(&convert_idle, K_FOREVER);

		char in_path[DATA_LOGGER_PATH_MAX];

		strncpy(in_path, convert_bin_path, sizeof(in_path) - 1);
		in_path[sizeof(in_path) - 1] = '\0';

#if defined(CONFIG_DATA_LOGGER_CONVERT_CSV)
		{
			char out_path[DATA_LOGGER_PATH_MAX];

			swap_extension(out_path, sizeof(out_path), in_path,
				       data_logger_csv_formatter.file_ext);
			int rc = data_logger_convert(in_path,
						     &data_logger_csv_formatter,
						     out_path);

			if (rc != 0) {
				LOG_ERR("CSV conversion of %s failed (%d)",
					in_path, rc);
			} else {
				LOG_INF("converted %s → %s", in_path, out_path);
			}
		}
#endif /* CONFIG_DATA_LOGGER_CONVERT_CSV */

#if defined(CONFIG_DATA_LOGGER_CONVERT_INFLUX)
		{
			char out_path[DATA_LOGGER_PATH_MAX];

			swap_extension(out_path, sizeof(out_path), in_path,
				       data_logger_influx_formatter.file_ext);
			int rc = data_logger_convert(in_path,
						     &data_logger_influx_formatter,
						     out_path);

			if (rc != 0) {
				LOG_ERR("Influx conversion of %s failed (%d)",
					in_path, rc);
			} else {
				LOG_INF("converted %s → %s", in_path, out_path);
			}
		}
#endif /* CONFIG_DATA_LOGGER_CONVERT_INFLUX */

		k_sem_give(&convert_idle);
	}
}

K_THREAD_DEFINE(converter_thread, 2048, converter_task, NULL, NULL, NULL,
		9, 0, 0);

static void log_begin_flight(void)
{
	if (k_sem_take(&convert_idle, K_MSEC(LOG_ARM_CONVERT_TIMEOUT_MS)) != 0) {
		LOG_ERR("ARMED: prior conversion still running after %d ms — "
			"flight will not be logged",
			LOG_ARM_CONVERT_TIMEOUT_MS);
#if defined(CONFIG_AURORA_NOTIFY)
		notify_error();
#endif
		return;
	}
	k_sem_give(&convert_idle);

	memset(&sm_logger, 0, sizeof(sm_logger));
	if (data_logger_init(&sm_logger, "flight",
			     &data_logger_bin_formatter) != 0) {
		LOG_ERR("data_logger_init failed");
#if defined(CONFIG_AURORA_NOTIFY)
		notify_error();
#endif
		return;
	}
	data_logger_set_default(&sm_logger);
	if (data_logger_start(&sm_logger) != 0) {
		LOG_ERR("data_logger_start failed");
		(void)data_logger_close(&sm_logger);
		data_logger_set_default(NULL);
#if defined(CONFIG_AURORA_NOTIFY)
		notify_error();
#endif
		return;
	}

	atomic_set(&sm_logger_live, 1);
}

static void log_end_flight(void)
{
	if (!atomic_get(&sm_logger_live)) {
		return;
	}
	atomic_set(&sm_logger_live, 0);

	(void)data_logger_stop(&sm_logger);

	strncpy(convert_bin_path, sm_logger.path, sizeof(convert_bin_path) - 1);
	convert_bin_path[sizeof(convert_bin_path) - 1] = '\0';

	(void)data_logger_close(&sm_logger);
	data_logger_set_default(NULL);

	k_sem_give(&convert_request);
}
#endif /* CONFIG_DATA_LOGGER */

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
static bool sm_active = false;   /**< True once the state machine thread has initialized. */

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

	LOG_INF("IMU ready");
}

/* Create the IMU task (inactive unless CONFIG_IMU=y) */
K_THREAD_DEFINE(imu_polling, 2048, imu_task, NULL, NULL, NULL,
				7, 0, 0);
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
	const int baro_hz = CONFIG_BARO_FREQUENCY_VALUE;
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
K_THREAD_DEFINE(baro_polling, 2048, baro_task, NULL, NULL, NULL,
				7, 0, 0);
#endif /* CONFIG_BARO && !CONFIG_AURORA_FAKE_SENSORS */

/* ============================================================
 *                     State machine TASK
 * ============================================================ */
#if defined(CONFIG_AURORA_STATE_MACHINE)

#if defined(CONFIG_AURORA_FAKE_SENSORS)
/* make the function known (defined in fake_sensors.c) */
void fake_sensors_on_calibrated(void);

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
	case SM_IDLE:      return (to == SM_ARMED);
	case SM_ARMED:     return (to == SM_BOOST);
	case SM_BOOST:     return (to == SM_BURNOUT);
	case SM_BURNOUT:   return (to == SM_APOGEE);
	case SM_APOGEE:    return (to == SM_MAIN || to == SM_ERROR);
	case SM_MAIN:      return (to == SM_REDUNDANT);
	case SM_REDUNDANT: return (to == SM_LANDED || to == SM_ERROR);
	case SM_ERROR:     return (to == SM_IDLE);
	case SM_LANDED:    return false;
	default:           return false;
	}
}
#endif /* CONFIG_AURORA_FAKE_SENSORS */

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
	double orientation[] = {
		0.0, 0.0, 0.0
	};
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
		LOG_ERR("Pyro device %s is not ready, trying again ...",
				pyro0->name);
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
				int64_t now_ns = (k_uptime_ticks() * NSEC_PER_SEC)
						 / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
				double dt_orient_s = (last_imu_ns != 0)
					? (double)(now_ns - last_imu_ns) / 1e9
					: 0.0;
				if (dt_orient_s < 0.0 || dt_orient_s > 1.0) {
					dt_orient_s = 0.0;
				}
				const double *bias_for_orient =
					attitude_is_calibrated(&attitude_state)
						? attitude_state.gyro_bias
						: NULL;
#else
				const double dt_orient_s = 0.0;
				const double *bias_for_orient = NULL;
#endif /* CONFIG_IMU */
				if (imu_sensor_value_to_orientation(&msg_buf.imu, dt_orient_s,
								    bias_for_orient,
								    orientation) == 0 &&
				    imu_sensor_value_to_acceleration(&msg_buf.imu, &acceleration) == 0) {
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
							attitude_calibrate_sample(&attitude_state,
										  accel_b, gyro_b);
							if (attitude_state.cal_samples >=
							    CONFIG_IMU_CALIBRATION_SAMPLES) {
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
							if (attitude_update(&attitude_state, accel_b,
									    gyro_b, dt_s, &a_v) == 0) {
								accel_vert = a_v;
							}
						}
					}
					last_imu_ns = now_ns;
				}
#endif /* CONFIG_IMU */
#if defined(CONFIG_DATA_LOGGER)
				uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
				struct datapoint dp = {
					.timestamp_ns = ts,
					.type = AURORA_DATA_IMU_ACCEL,
					.channel_count = 3,
					.channels = {
						msg_buf.imu.accel[0],
						msg_buf.imu.accel[1],
						msg_buf.imu.accel[2]
					},
				};
				log_enqueue(&dp);

				dp.type = AURORA_DATA_IMU_GYRO;
				dp.channels[0] = msg_buf.imu.gyro[0];
				dp.channels[1] = msg_buf.imu.gyro[1];
				dp.channels[2] = msg_buf.imu.gyro[2];
				log_enqueue(&dp);
#endif /* CONFIG_DATA_LOGGER */
			} else if (data_chan == &baro_data_chan) {
#if defined(CONFIG_DATA_LOGGER)
				uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
				struct datapoint dp = {
					.timestamp_ns = ts,
					.type = AURORA_DATA_BARO,
					.channel_count = 2,
					.channels = {msg_buf.baro.temperature, msg_buf.baro.pressure},
				};
				log_enqueue(&dp);
#endif /* CONFIG_DATA_LOGGER */
				if (baro_sensor_value_to_altitude(&msg_buf.baro.pressure, &altitude) == 0) {
					baro_ready = true;
				}
			}
		} while (zbus_sub_wait_msg(&sm_sub, &data_chan, &msg_buf, K_NO_WAIT) == 0);

		/* Only update state machine once both sensors have fresh data */
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

#if defined(CONFIG_DATA_LOGGER)
		{
			struct sm_inputs sm_in;
			sm_get_inputs(&sm_in);

			uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
			struct datapoint kin_dp = {
				.timestamp_ns = ts,
				.type = AURORA_DATA_SM_KINEMATICS,
				.channel_count = 2,
			};
			sensor_value_from_double(&kin_dp.channels[0], sm_in.acceleration);
			sensor_value_from_double(&kin_dp.channels[1], sm_in.accel_vert);
			log_enqueue(&kin_dp);

			struct datapoint pose_dp = {
				.timestamp_ns = ts,
				.type = AURORA_DATA_SM_POSE,
				.channel_count = 2,
			};
			sensor_value_from_double(&pose_dp.channels[0], sm_in.velocity);
			sensor_value_from_double(&pose_dp.channels[1], sm_in.altitude);
			log_enqueue(&pose_dp);

			struct datapoint or_dp = {
				.timestamp_ns = ts,
				.type = AURORA_DATA_ORIENTATION,
				.channel_count = 3,
			};
			sensor_value_from_double(&or_dp.channels[0], sm_in.orientation[0]);
			sensor_value_from_double(&or_dp.channels[1], sm_in.orientation[1]);
			sensor_value_from_double(&or_dp.channels[2], sm_in.orientation[2]);
			log_enqueue(&or_dp);
		}
#endif /* CONFIG_DATA_LOGGER */

		if (state != prev_state) {
#if defined(CONFIG_AURORA_FAKE_SENSORS)
			__ASSERT(is_valid_transition(prev_state, state),
				 "Invalid SM transition: %s -> %s",
				 sm_state_str(prev_state), sm_state_str(state));
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
#if defined(CONFIG_DATA_LOGGER)
			/* Flight-time logging lifecycle:
			 *  - begin on IDLE→ARMED (opens a fresh binary file),
			 *  - end on exit to IDLE/LANDED/ERROR (kicks converter).
			 */
			if (prev_state == SM_IDLE && state == SM_ARMED) {
				log_begin_flight();
			} else if (state == SM_IDLE || state == SM_LANDED ||
				   state == SM_ERROR) {
				log_end_flight();
			}
#endif /* CONFIG_DATA_LOGGER */
			prev_state = state;
		}

		/* reset the measurements */
		baro_ready = false;
		imu_ready = false;

#if defined(CONFIG_PYRO)
		if (state == pyro_state)
			continue;

#define PYRO_ACT(fn, ch, past, action)					\
		do {							\
			if (fn(pyro0, ch))				\
				LOG_ERR("Failed to " action		\
					" pyro0 channel " #ch);	\
			else						\
				LOG_INF(past " pyro0 channel " #ch);\
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
		case SM_REDUNDANT:
			PYRO_ACT(pyro_trigger_channel, 1, "Re-triggered", "re-trigger");
			break;
		default:
			break;
		}
#undef PYRO_ACT
		pyro_state = state;
#endif /* CONFIG_PYRO */

	}
}

/* Create the State machine task */
K_THREAD_DEFINE(state_machine, 4096, state_machine_task, NULL, NULL,
				NULL, 6, 0, 0);
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

	return 0;
}
