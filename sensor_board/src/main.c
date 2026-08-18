/**
 * @file main.c
 * @brief Sensor board application entry point.
 *
 * One thread owns the flight: it reads each sensor as it comes due and runs
 * the state machine off the result.
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

#include <zephyr/app_version.h>

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif /* CONFIG_HWINFO */

#if defined(CONFIG_IMU)
#include <aurora/lib/attitude.h>
#include <aurora/lib/imu.h>
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#include <aurora/lib/baro.h>
#endif /* CONFIG_BARO */

#if defined(CONFIG_AURORA_FAKE_SENSORS)
#include <aurora/lib/sim.h>
#endif /* CONFIG_AURORA_FAKE_SENSORS */

#if defined(CONFIG_AURORA_SIM_AUTOTEST)
#include <math.h>
#include <stdlib.h>
#include <zephyr/logging/log_ctrl.h>
#endif /* CONFIG_AURORA_SIM_AUTOTEST */

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

#if defined(CONFIG_AURORA_WATCHDOG)
#include <aurora/lib/watchdog.h>
#define WDT_KICK(src) aurora_watchdog_kick(src)
#else
#define WDT_KICK(src)
#endif /* CONFIG_AURORA_WATCHDOG */

#if defined(CONFIG_AURORA_PAD_LINK)
#include <aurora/lib/pad_link.h>
#endif /* CONFIG_AURORA_PAD_LINK */

#if defined(CONFIG_AURORA_STATE_MACHINE)
#include <aurora/lib/state/config.h>
#include <aurora/lib/state/state.h>

#if defined(CONFIG_AURORA_STATE_MACHINE_RETAIN)
#include <aurora/lib/state/retain.h>

#if defined(CONFIG_IMU)
/* The save call below discards its return value, so an attitude struct that
 * outgrew the payload would stop being retained without a word -- and the
 * symptom (a recovered flight that cannot see vertical acceleration) would
 * only show up in the air.  Fail the build instead.
 */
BUILD_ASSERT(sizeof(struct attitude) <= SM_RETAIN_BLOB_SIZE,
	     "struct attitude no longer fits in the retained payload; "
	     "raise SM_RETAIN_BLOB_SIZE in retain.h");
#endif /* CONFIG_IMU */
#endif /* CONFIG_AURORA_STATE_MACHINE_RETAIN */

static struct sm_thresholds state_cfg;
#endif /* CONFIG_AURORA_STATE_MACHINE */

LOG_MODULE_REGISTER(main, CONFIG_SENSOR_BOARD_LOG_LEVEL);

/* ============================================================
 *                     SENSORS
 * ============================================================ */

/* Every sensor is read from the state machine thread below, one at a time.
 *
 * The sensors share a single I2C controller, and a controller driver may
 * serialise concurrent transfers by spinning rather than blocking: the esp32
 * controller opens i2c_esp32_transfer() with a k_busy_wait loop on its
 * bus-busy flag lasting up to 500 ms, before taking its own transfer
 * semaphore.  That loop does not yield, so a second poller would spend the
 * overlap of every colliding transfer burning CPU rather than sleeping.
 * Owning the bus from one thread removes the collision outright.
 *
 * A sensor driven by a data-ready trigger is read on the driver's own thread
 * instead -- forced by the driver, not chosen; see the handler in imu.c -- but
 * only far enough to get the sample off the chip.  Everything downstream of
 * that still runs here.
 */

/** Sleep cap when no sensor has a deadline, e.g. every one failed to init. */
#define SENSOR_IDLE_SLEEP_MS 1000

#if defined(CONFIG_IMU)
#define IMU_PERIOD_MS MAX(1, 1000 / CONFIG_IMU_FREQUENCY)

#if defined(CONFIG_AURORA_FAKE_SENSORS)
#define IMU_DEV NULL
#else
#if !DT_HAS_CHOSEN(auxspace_imu)
#error "CONFIG_IMU requires DT chosen 'auxspace,imu' to point at an IMU sensor node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_imu), okay),
	     "the 'auxspace,imu' chosen node must have status \"okay\"");
#define IMU_DEV DEVICE_DT_GET(DT_CHOSEN(auxspace_imu))
#endif /* CONFIG_AURORA_FAKE_SENSORS */

static bool imu_ok;
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#define BARO_PERIOD_MS MAX(1, 1000 / CONFIG_BARO_FREQUENCY)

#if defined(CONFIG_AURORA_FAKE_SENSORS)
#define BARO_DEV NULL
#else
#if !DT_HAS_CHOSEN(auxspace_baro)
#error "CONFIG_BARO requires DT chosen 'auxspace,baro' to point at a baro sensor node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_baro), okay),
	     "the 'auxspace,baro' chosen node must have status \"okay\"");
#define BARO_DEV DEVICE_DT_GET(DT_CHOSEN(auxspace_baro))
#endif /* CONFIG_AURORA_FAKE_SENSORS */

static bool baro_ok;
#endif /* CONFIG_BARO */

/**
 * @brief Bring every configured sensor up.
 *
 * A sensor that fails to initialize is left out rather than taking the others
 * down with it; one that comes up but cannot deliver its data-ready trigger
 * falls back to being polled, which is degraded but still flyable.
 */
static void sensors_init(void)
{
#if defined(CONFIG_IMU)
	int imu_rc = imu_init(IMU_DEV);

	if (imu_rc == -ENOTSUP) {
		LOG_WRN("IMU data-ready trigger unavailable; polling at %d Hz",
			CONFIG_IMU_FREQUENCY);
		imu_rc = 0;
	}

	imu_ok = (imu_rc == 0);
	if (imu_ok) {
		LOG_INF("IMU ready");
	} else {
		LOG_ERR("IMU not ready!");
	}
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
	int baro_rc = baro_init(BARO_DEV);

	if (baro_rc == -ENOTSUP) {
		LOG_WRN("Baro data-ready trigger unavailable; polling at %d Hz",
			CONFIG_BARO_FREQUENCY);
		baro_rc = 0;
	}

	baro_ok = (baro_rc == 0);
	if (baro_ok) {
		LOG_INF("Baro ready");
	} else {
		LOG_ERR("Baro not ready!");
	}
#endif /* CONFIG_BARO */
}

/* ============================================================
 *                     State machine TASK
 * ============================================================ */
#if defined(CONFIG_AURORA_STATE_MACHINE)

/* Flight inputs derived from the samples.  File-scope because exactly one
 * thread owns them, which is also what lets the helpers below take no
 * arguments.
 */
static double altitude;
static double acceleration;
static double accel_vert;
static double orientation[3];
static bool imu_ready;
static bool baro_ready;
/* No-IMU builds have nothing to calibrate, so treat calibration as instantly
 * satisfied; CONFIG_IMU builds overwrite this from the attitude tracker.
 */
static bool calibrated = true;

#if defined(CONFIG_IMU)
static struct attitude attitude_state;
static int64_t last_imu_ns;
static bool calibration_started_notified;
static bool calibration_notified;
#endif /* CONFIG_IMU */

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

/**
 * @brief Drive and adjudicate an unattended simulated flight.
 *
 * Launches as soon as attitude calibration converges, then exits the process
 * on the outcome so CI can read it off the exit code.  Runs on the state
 * machine thread: the flight it is watching cannot advance without it.
 */
static void autotest_step(enum sm_state state)
{
	static bool launched;
	static int64_t deadline;

	if (!launched) {
		if (!calibrated) {
			return;
		}

		LOG_INF("autolaunch: calibration complete, launching");
		sim_launch();
		deadline = k_uptime_get() + CONFIG_AURORA_SIM_AUTOLAUNCH_TIMEOUT_MS;
		launched = true;
		return;
	}

	if (state == SM_LANDED) {
#if !defined(CONFIG_AURORA_FAKE_SENSORS_REPLAY)
		/* The synthetic rocket stays vertical and spins about its up
		 * axis, so yaw and pitch must have held while roll moved.
		 */
		LOG_INF("autolaunch: orientation yaw=%.2f pitch=%.2f roll=%.2f",
			orientation[0], orientation[1], orientation[2]);
		__ASSERT(fabs(orientation[0]) < 5.0,
			 "autolaunch: yaw drift %.2f deg exceeds 5 deg",
			 orientation[0]);
		__ASSERT(fabs(orientation[1]) < 5.0,
			 "autolaunch: pitch drift %.2f deg exceeds 5 deg",
			 orientation[1]);
		__ASSERT(fabs(orientation[2]) > 1.0,
			 "autolaunch: roll did not integrate (%.2f deg)",
			 orientation[2]);
#endif /* !CONFIG_AURORA_FAKE_SENSORS_REPLAY */
		LOG_INF("autolaunch: LANDED - simulation complete");
		log_flush();
		exit(0);
	}

	if (state == SM_ERROR) {
		/* The __ASSERT in the error handler likely fires first */
		LOG_ERR("autolaunch: ERROR state - simulation failed");
		log_flush();
		exit(1);
	}

	if (k_uptime_get() >= deadline) {
		LOG_ERR("autolaunch: timeout after %d ms without landing",
			CONFIG_AURORA_SIM_AUTOLAUNCH_TIMEOUT_MS);
		log_flush();
		exit(1);
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

#if defined(CONFIG_IMU)
/**
 * @brief Turn a raw IMU sample into orientation, acceleration and attitude.
 *
 * Tracks calibration while the vehicle is IDLE, and computes earth-frame
 * vertical acceleration once calibrated.
 *
 * @param imu_data Incoming raw IMU sample.
 */
static void handle_imu(const struct imu_data *imu_data)
{
	int64_t now_ns = (k_uptime_ticks() * NSEC_PER_SEC) / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	/* Delta-time since the last sample, clamped to a sane range; 0.0 on the
	 * first sample or after a discontinuity.
	 */
	double dt_s = (last_imu_ns != 0) ? (double)(now_ns - last_imu_ns) / 1e9 : 0.0;

	if (dt_s < 0.0 || dt_s > 1.0) {
		dt_s = 0.0;
	}

	const double *bias_for_orient =
		attitude_is_calibrated(&attitude_state) ? attitude_state.gyro_bias : NULL;

	if (imu_sensor_value_to_orientation(imu_data, dt_s, bias_for_orient, orientation) == 0
	    && imu_sensor_value_to_acceleration(imu_data, &acceleration) == 0) {
		imu_ready = true;
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

	if (!attitude_is_calibrated(&attitude_state)) {
		if (sm_get_state() == SM_IDLE) {
			attitude_calibrate_sample(&attitude_state, accel_b, gyro_b);
#if defined(CONFIG_AURORA_NOTIFY)
			if (!calibration_started_notified &&
			    attitude_state.cal_samples > 0) {
				notify_calibration_start();
				calibration_started_notified = true;
			}
#endif /* CONFIG_AURORA_NOTIFY */
			if (attitude_calibrate_converged(&attitude_state) &&
			    attitude_calibrate_finish(&attitude_state) == 0) {
#if defined(CONFIG_AURORA_STATE_MACHINE_RETAIN)
				/* Snapshot it now, while the vehicle is still
				 * stationary and the result is known good.
				 * Calibration only runs in SM_IDLE, so a board
				 * that reboots into a flight state can never
				 * redo this -- it has to be handed the old
				 * answer.
				 */
				(void)sm_retain_save_blob(&attitude_state,
							  sizeof(attitude_state));
#endif /* CONFIG_AURORA_STATE_MACHINE_RETAIN */
#if defined(CONFIG_AURORA_NOTIFY)
				if (!calibration_notified) {
					notify_calibration_complete();
					calibration_notified = true;
				}
#endif /* CONFIG_AURORA_NOTIFY */
			}
		}
		accel_vert = 0.0;
	} else if (dt_s > 0.0) {
		double a_v;

		if (attitude_update(&attitude_state, accel_b, gyro_b, dt_s, &a_v) == 0) {
			accel_vert = a_v;
		}
	}

	last_imu_ns = now_ns;
}
#endif /* CONFIG_IMU */

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
	/* Calibration runs in the background while IDLE, but pyros must not
	 * go live until SM_ARMED; SM_ERROR is only ever held pre-flight
	 * (arming interlock). Keep the channels safe in both, explicit
	 * rather than relying on the default no-op case, for auditability
	 * of this flight-critical switch.
	 */
	case SM_IDLE:
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

static void handle_state_transition(enum sm_state prev_state, enum sm_state state)
{
#if defined(CONFIG_AURORA_SIM_AUTOTEST)
	__ASSERT(is_valid_transition(prev_state, state),
		"Invalid SM transition: %s -> %s",
		sm_state_str(prev_state),
		sm_state_str(state));
#endif /* CONFIG_AURORA_SIM_AUTOTEST */
#if defined(CONFIG_AURORA_NOTIFY)
	notify_state_change(prev_state, state);
#endif /* CONFIG_AURORA_NOTIFY */
#if defined(CONFIG_IMU)
	/* On return to IDLE, discard calibration so a re-arm triggers a fresh
	 * stationary calibration window.
	 */
	if (state == SM_IDLE) {
		attitude_init(&attitude_state);
		last_imu_ns = 0;
		calibration_started_notified = false;
		calibration_notified = false;
		orientation[2] = 0.0;
	}
#endif /* CONFIG_IMU */
	log_handle_flight_lifecycle(prev_state, state);
}

/**
 * @brief Flight thread: reads every sensor and runs the state machine off it.
 */
void state_machine_task(void *, void *, void *)
{
	enum sm_state state;
	enum sm_state prev_state = SM_IDLE;

	struct sm_error_handling_args sm_error_handler = {
		.cb = &state_machine_error_handler,
		.args = NULL,
	};

#if defined(CONFIG_IMU)
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
#endif /* CONFIG_PYRO */

	sensors_init();

	/* Per-vehicle thresholds with Kconfig fallbacks */
	sm_config_load(&state_cfg);

	sm_init(&state_cfg, &sm_error_handler);

#if defined(CONFIG_IMU) && defined(CONFIG_AURORA_STATE_MACHINE_RETAIN)
	if (sm_retain_recovered()) {
		/* Hand back the calibration from before the reset */
		if (sm_retain_load_blob(&attitude_state, sizeof(attitude_state)) == 0) {
			LOG_WRN("restored attitude calibration from before the reset");
		} else {
			/* Nothing to restore */
			LOG_ERR("no attitude calibration retained; vertical "
				"acceleration and orientation are unavailable "
				"for the rest of this flight");
		}
	}
#endif /* CONFIG_IMU && CONFIG_AURORA_STATE_MACHINE_RETAIN */

#if defined(CONFIG_AURORA_STATE_MACHINE_RETAIN)
	/* Replay the IDLE => resumed edge for the consumers that are driven by
	 * transitions and therefore still believe the vehicle is IDLE after a
	 * silent restore.
	 *
	 * Deliberately narrower than handle_state_transition(): replaying its
	 * log lifecycle wholesale would, on a recovery into LANDED, schedule a
	 * close for a log that was never opened, which is why it was excluded
	 * outright.  log_resume_flight_after_reset() covers just the airborne
	 * states, where a fresh log is what the flight needs.
	 *
	 * handle_pyro() stays excluded and still owes an audit of its own: it
	 * seeds pyro_state to SM_IDLE, so a recovery into APOGEE/MAIN/
	 * REDUNDANT re-fires a channel on the loop's first pass.
	 */
	if (sm_retain_recovered()) {
		enum sm_state resumed = sm_get_state();

		if (resumed != prev_state) {
			LOG_WRN("resumed in %s after reset; re-syncing "
				"notifications and flight recording",
				sm_state_str(resumed));
#if defined(CONFIG_AURORA_NOTIFY)
			notify_state_change(prev_state, resumed);
#endif /* CONFIG_AURORA_NOTIFY */
			log_resume_flight_after_reset(resumed);
			prev_state = resumed;
		}
	}
#endif /* CONFIG_AURORA_STATE_MACHINE_RETAIN */

	int64_t now = k_uptime_get();
#if defined(CONFIG_IMU)
	int64_t imu_due = now;
	struct imu_data imu_msg;
#endif /* CONFIG_IMU */
#if defined(CONFIG_BARO)
	int64_t baro_due = now;
	struct baro_data baro_msg;
#endif /* CONFIG_BARO */

	while (1) {
		now = k_uptime_get();
		/* Seed for the "earliest deadline" search below. */
		int64_t next_due = now + SENSOR_IDLE_SLEEP_MS;

#if defined(CONFIG_IMU)
		if (imu_ok) {
			if (now >= imu_due) {
				/* -EAGAIN only means a triggered sensor has
				 * nothing new; a real sample is handled here.
				 */
				if (imu_poll(IMU_DEV, &imu_msg) == 0) {
					WDT_KICK(AURORA_WDT_SRC_IMU);
					handle_imu(&imu_msg);
					log_imu_data(&imu_msg);
				}

				/* A single read can take hundreds of ms on a
				 * sick bus, so re-read the clock rather than
				 * pacing the barometer off a stale one.
				 */
				now = k_uptime_get();
				imu_due += IMU_PERIOD_MS;
				if (imu_due <= now) {
					imu_due = now + IMU_PERIOD_MS;
				}
			}

			next_due = MIN(next_due, imu_due);
		}
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
		if (baro_ok) {
			if (now >= baro_due) {
				if (baro_measure(BARO_DEV, &baro_msg) == 0) {
					WDT_KICK(AURORA_WDT_SRC_BARO);
					log_baro_data(&baro_msg);

					if (baro_sensor_value_to_altitude(
						    &baro_msg.pressure, &altitude) == 0) {
						baro_ready = true;
					}
				}

				now = k_uptime_get();
				baro_due += BARO_PERIOD_MS;
				if (baro_due <= now) {
					baro_due = now + BARO_PERIOD_MS;
				}
			}

			next_due = MIN(next_due, baro_due);
		}
#endif /* CONFIG_BARO */

#if defined(CONFIG_IMU)
		calibrated = attitude_is_calibrated(&attitude_state) > 0;
#endif /* CONFIG_IMU */

		/* Hold the state machine until both sensors have reported once,
		 * so it is never updated with uninitialized data during
		 * startup.  After that the Kalman filter inside it copes fine
		 * with an update carrying only one fresh sensor.
		 */
		if (baro_ready && imu_ready) {
			struct sm_inputs inputs = {
				.armed = 1,
				.log_ready = log_flight_log_online(),
				.log_busy = log_flight_log_busy(),
				.calibrated = calibrated,
				.acceleration = acceleration,
				.accel_vert = accel_vert,
				.altitude = altitude,
			};
			memcpy(inputs.orientation, orientation, sizeof(inputs.orientation));

			sm_update(&inputs);
			state = sm_get_state();

			/* Deliberately here and not at the top of the loop:
			 * reaching this point means both sensors delivered and
			 * the machine actually advanced.  A kick at the loop
			 * head would keep looking alive while the gate above
			 * waits for a sensor that never comes back.
			 */
			WDT_KICK(AURORA_WDT_SRC_STATE);

			/* update pad link data */
			update_pad_link_data();

			log_flight_telemetry();
			log_vbat_telemetry();

			if (state != prev_state) {
				handle_state_transition(prev_state, state);
				prev_state = state;
			}

			/* reset the measurements */
			baro_ready = false;
			imu_ready = false;

			handle_pyro(state, &pyro_state, pyro0);
		}

#if defined(CONFIG_AURORA_SIM_AUTOTEST)
		autotest_step(sm_get_state());
#endif /* CONFIG_AURORA_SIM_AUTOTEST */

		if (next_due <= now) {
			/* Unreachable: every sensor reaching the search above
			 * has just advanced its own deadline past `now`.  Kept
			 * as a floor because the alternative is a spin that
			 * starves every thread below this one.
			 */
			next_due = now + 1;
		}

		/* Absolute deadline, so a slow read eats into this sleep
		 * instead of adding to the period.
		 */
		k_sleep(K_TIMEOUT_ABS_MS(next_due));
	}
}

/* Preemptible on purpose, and placed above the loggers (8, 9) so an SD stall
 * cannot cost sensor samples, below the drivers' cooperative trigger threads,
 * and above the watchdog feeder (10) -- if this thread ever spins, the feeder
 * starves and the board resets, which is the correct outcome.
 */
K_THREAD_DEFINE(state_machine, 4096, state_machine_task, NULL, NULL, NULL, 6, 0, 0);
#endif /* CONFIG_AURORA_STATE_MACHINE */

/* ============================================================
 *                     MAIN INITIALIZATION
 * ============================================================ */
#if defined(CONFIG_HWINFO)
/**
 * @brief Report and clear the cause of the last reset.
 *
 * A console cannot tell a panic from a brownout, a watchdog bite or a flat
 * pack: every one of them just stops the log mid-line. The SoC's reset
 * latch can, and after an unattended run it is the first thing worth
 * knowing -- it decides whether to go looking for a coredump or for a
 * power problem.
 *
 * Reported per set bit rather than as a raw mask so the answer is legible
 * in the field with no console attached to decode it. The latch is sticky
 * across resets, so it is cleared afterwards: otherwise every later boot
 * keeps re-reporting the same stale cause.
 */
static void log_reset_cause(void)
{
	static const struct {
		uint32_t flag;
		const char *name;
	} causes[] = {
		{RESET_PIN, "reset pin"},
		{RESET_SOFTWARE, "software"},
		{RESET_BROWNOUT, "brownout"},
		{RESET_POR, "power-on"},
		{RESET_WATCHDOG, "watchdog"},
		{RESET_DEBUG, "debugger"},
		{RESET_SECURITY, "security violation"},
		{RESET_LOW_POWER_WAKE, "low-power wake"},
		{RESET_CPU_LOCKUP, "CPU lockup"},
		{RESET_PARITY, "parity error"},
		{RESET_PLL, "PLL error"},
		{RESET_CLOCK, "clock error"},
		{RESET_HARDWARE, "hardware"},
		{RESET_USER, "user"},
		{RESET_TEMPERATURE, "temperature"},
	};
	uint32_t cause = 0;
	int rc = hwinfo_get_reset_cause(&cause);

	if (rc != 0) {
		LOG_WRN("Reset cause unavailable (%d)", rc);
		return;
	}

	if (cause == 0) {
		LOG_INF("Reset cause: not reported");
		return;
	}

	/* A cold start or a button press is how the board is *meant* to come
	 * up. Anything else means the previous run ended badly and should
	 * stand out in a scrollback.
	 */
	bool unexpected = (cause & ~(uint32_t)(RESET_POR | RESET_PIN)) != 0;

	for (size_t i = 0; i < ARRAY_SIZE(causes); i++) {
		if ((cause & causes[i].flag) == 0) {
			continue;
		}
		if (unexpected) {
			LOG_WRN("Reset cause: %s", causes[i].name);
		} else {
			LOG_INF("Reset cause: %s", causes[i].name);
		}
	}

	if (unexpected) {
		LOG_WRN("Previous run ended abnormally; check 'coredump find'");
	}

	(void)hwinfo_clear_reset_cause();
}
#endif /* CONFIG_HWINFO */

/**
 * @brief Application entry point.
 *
 * Logs the firmware version. All work is performed by threads started
 * automatically via K_THREAD_DEFINE.
 */
int main(void)
{
	LOG_INF("Auxspace AURORA %s", APP_VERSION_STRING);

#if defined(CONFIG_HWINFO)
	/* Before any subsystem brings itself up and floods the console: this
	 * line explains the boot that is about to happen.
	 */
	log_reset_cause();
#endif /* CONFIG_HWINFO */

#if defined(CONFIG_AURORA_NOTIFY)
	notify_init();
	notify_boot();
#endif /* CONFIG_AURORA_NOTIFY */

#if defined(CONFIG_AURORA_WATCHDOG)
	(void)aurora_watchdog_setup();
#endif /* CONFIG_AURORA_WATCHDOG */

#if defined(CONFIG_AURORA_POWERFAIL)
	/* Samples its input during setup and can raise a notification straight
	 * away, so it comes up behind the indicators.
	 *
	 * No app-level hooks: the library already stops the data loggers and
	 * raises the notification. The flight state machine deliberately keeps
	 * running - a sagging pack must not safe the recovery charges
	 * mid-flight.
	 */
	(void)powerfail_setup(NULL, NULL);
#endif /* CONFIG_AURORA_POWERFAIL */

#if defined(CONFIG_AURORA_TELEMETRY)
	(void)telemetry_init();
#endif /* CONFIG_AURORA_TELEMETRY */

#if defined(CONFIG_AURORA_PAD_LINK)
	/* This app's sensor requirements: the IMU is measuring 6-DoF and the
	 * baro doubles as an inner temperature sensor. Declared here
	 * because it is hardware knowledge the pad-link library must not
	 * hard code. Set before init so the boardcap register is valid
	 * the moment a central can connect.
	 */
	uint32_t pl_caps = 0;
#if defined(CONFIG_IMU)
	pl_caps |= PL_CAP_IMU_TYPE(PL_CAP_IMU_TYPE_6DOF) |
		   PL_CAP_ACCEL | PL_CAP_GYRO;
#endif /* CONFIG_IMU */
#if defined(CONFIG_BARO)
	pl_caps |= PL_CAP_BARO | PL_CAP_TEMP_INNER;
#endif /* CONFIG_BARO */
	pad_link_set_caps(pl_caps);
	(void)pad_link_init();
#endif /* CONFIG_AURORA_PAD_LINK */

	return 0;
}
