/**
 * @file main.c
 * @brief Sensor board application entry point.
 *
 * Defines two Zephyr threads that run concurrently: one owns the sensor bus
 * and publishes samples, the other drives the flight state machine from them.
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

ZBUS_MSG_SUBSCRIBER_DEFINE(sm_sub);

#if defined (CONFIG_IMU)
ZBUS_CHAN_ADD_OBS(imu_data_chan, sm_sub, 1);
#endif

#if defined (CONFIG_BARO)
ZBUS_CHAN_ADD_OBS(baro_data_chan, sm_sub, 1);
#endif

bool baro_active = false; /**< True once the barometer thread has initialized. */
bool imu_active = false;  /**< True once the IMU thread has initialized. */
static bool sm_active = false; /**< True once the state machine thread has initialized. */

/* ============================================================
 *                     SENSOR TASK
 * ============================================================ */
#if (defined(CONFIG_IMU) || defined(CONFIG_BARO)) && !defined(CONFIG_AURORA_FAKE_SENSORS)
/* One thread services every polled sensor, in turn, rather than one thread
 * per sensor.
 *
 * The sensors share a single I2C controller, and a controller driver may
 * serialise concurrent transfers by spinning rather than blocking: the
 * esp32 controller opens i2c_esp32_transfer() with a k_busy_wait loop on its
 * bus-busy flag lasting up to I2C_TRANSFER_TIMEOUT_MSEC (500 ms), and it does
 * so before taking its own transfer semaphore.
 * That loop does not yield, so with one poller per sensor the two spend the
 * overlap of every colliding transfer burning CPU at each other instead of
 * sleeping.
 * And a slave that wedges SDA turns each attempt into 500 ms during which
 * nothing of lower priority is scheduled at all, the watchdog feeder included.
 *
 * Owning the bus from a single thread removes the collision outright, and
 * makes the fault path serial so the back-off below can actually pace it.
 * Sensors driven by a data-ready trigger publish from the driver's own
 * trigger thread and are only initialized here.
 */
#if defined(CONFIG_IMU)
/* Build-time dependency: the sensor task fetches its device from the
 * 'auxspace,imu' chosen node. */
#if !DT_HAS_CHOSEN(auxspace_imu)
#error "CONFIG_IMU requires DT chosen 'auxspace,imu' to point at an IMU sensor node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_imu), okay),
	     "the 'auxspace,imu' chosen node must have status \"okay\"");
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
/* Likewise for the barometer's 'auxspace,baro' chosen node. */
#if !DT_HAS_CHOSEN(auxspace_baro)
#error "CONFIG_BARO requires DT chosen 'auxspace,baro' to point at a baro sensor node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_baro), okay),
	     "the 'auxspace,baro' chosen node must have status \"okay\"");
#endif /* CONFIG_BARO */

#if (defined(CONFIG_IMU) && !defined(CONFIG_IMU_TRIGGER)) ||                                       \
	(defined(CONFIG_BARO) && !defined(CONFIG_BARO_TRIGGER))
#define AURORA_HAVE_POLLED_SENSOR 1
#endif

#if defined(AURORA_HAVE_POLLED_SENSOR)
/* Retry spacing after a failed fetch.
 *
 * A wedged bus costs up to 500 ms of non-yielding busy-wait per attempt (see
 * above), so retrying at the nominal poll rate would hold a CPU for ~83% of
 * the time the wedge lasts and starve the flight state machine, the loggers
 * and the watchdog feeder.  Doubling the gap bounds that duty cycle to a
 * level the feeder always survives, and as a side effect spaces attempts out
 * past AURORA_BUS_RECOVER_MIN_INTERVAL_MS so the bus-recovery call in the
 * sensor wrappers is actually reached instead of being rate-limited away.
 */
#define SENSOR_BACKOFF_MIN_MS 100
#define SENSOR_BACKOFF_MAX_MS 2000

/* Upper bound on one sleep, not a poll rate.  The loop sleeps until the
 * earliest slot deadline, and this is only the starting value that search
 * minimises against -- at CONFIG_IMU_FREQUENCY=100 the IMU slot pulls it
 * straight down to 10 ms.  It survives only when every slot is dead (each
 * dev NULL after a failed init), where it parks the loop at 1 Hz rather
 * than letting it spin.
 */
#define SENSOR_MAX_SLEEP_MS 1000

/** One polled sensor and its schedule. */
struct sensor_slot {
	const struct device *dev;            /**< NULL once init has failed. */
	int (*fetch)(const struct device *); /**< Sample and publish. */
	const char *name;                    /**< For diagnostics. */
	uint32_t period_ms;                  /**< Nominal poll period. */
	int64_t due_ms;                      /**< Absolute uptime deadline. */
	uint32_t backoff_ms;                 /**< 0 while healthy. */
};

enum {
#if defined(CONFIG_IMU) && !defined(CONFIG_IMU_TRIGGER)
	SLOT_IMU,
#endif
#if defined(CONFIG_BARO) && !defined(CONFIG_BARO_TRIGGER)
	SLOT_BARO,
#endif
	SLOT_COUNT,
};

/* File-scope rather than automatic: one thread owns this, and it keeps the
 * schedule off a stack that on Xtensa also absorbs register-window spills.
 */
static struct sensor_slot sensor_slots[SLOT_COUNT] = {
#if defined(CONFIG_IMU) && !defined(CONFIG_IMU_TRIGGER)
	[SLOT_IMU] = {
		.fetch = imu_poll,
		.name = "IMU",
		.period_ms = MAX(1, 1000 / CONFIG_IMU_FREQUENCY),
	},
#endif
#if defined(CONFIG_BARO) && !defined(CONFIG_BARO_TRIGGER)
	[SLOT_BARO] = {
		.fetch = baro_measure,
		.name = "baro",
		.period_ms = MAX(1, 1000 / CONFIG_BARO_FREQUENCY),
	},
#endif
};

/**
 * @brief Advance a slot's deadline after an attempt.
 *
 * @param slot Slot that was just polled.
 * @param now  Uptime in ms, sampled after the fetch returned.
 * @param ok   Whether the fetch succeeded.
 */
static void sensor_slot_reschedule(struct sensor_slot *slot, int64_t now, bool ok)
{
	if (!ok) {
		slot->backoff_ms = (slot->backoff_ms == 0)
					   ? SENSOR_BACKOFF_MIN_MS
					   : MIN(slot->backoff_ms * 2U,
						 (uint32_t)SENSOR_BACKOFF_MAX_MS);
		slot->due_ms = now + slot->backoff_ms;
		return;
	}

	if (slot->backoff_ms != 0) {
		LOG_INF("%s recovered after %u ms back-off", slot->name, slot->backoff_ms);
		slot->backoff_ms = 0;
	}

	slot->due_ms += slot->period_ms;
	if (slot->due_ms <= now) {
		slot->due_ms = now + slot->period_ms;
	}
}
#endif /* AURORA_HAVE_POLLED_SENSOR */

/**
 * @brief Sensor thread.
 *
 * Initializes every configured sensor, then polls the ones that are not
 * trigger-driven at their configured frequencies, publishing each sample to
 * its zbus channel.
 */
void sensor_task(void *, void *, void *)
{
#if defined(CONFIG_IMU)
	const struct device *imu0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_imu));

	if (imu_init(imu0) == 0) {
		imu_active = true;
		LOG_INF("IMU ready");
	} else {
		LOG_ERR("IMU not ready!");
		imu0 = NULL;
	}
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
	const struct device *baro0 = DEVICE_DT_GET(DT_CHOSEN(auxspace_baro));

	if (baro_init(baro0) == 0) {
		baro_active = true;
		LOG_INF("Baro ready");
	} else {
		LOG_ERR("Baro not ready!");
		baro0 = NULL;
	}
#endif /* CONFIG_BARO */

#if defined(AURORA_HAVE_POLLED_SENSOR)
#if defined(CONFIG_IMU) && !defined(CONFIG_IMU_TRIGGER)
	sensor_slots[SLOT_IMU].dev = imu0;
#endif
#if defined(CONFIG_BARO) && !defined(CONFIG_BARO_TRIGGER)
	sensor_slots[SLOT_BARO].dev = baro0;
#endif

	while (1) {
		int64_t now = k_uptime_get();
		/* Seed for the "earliest deadline" search below. */
		int64_t next_due = now + SENSOR_MAX_SLEEP_MS;

		for (size_t i = 0; i < SLOT_COUNT; i++) {
			struct sensor_slot *slot = &sensor_slots[i];

			if (slot->dev == NULL) {
				continue;
			}

			if (slot->due_ms <= now) {
				bool ok = (slot->fetch(slot->dev) == 0);

				/* A single fetch can take hundreds of ms on a
				 * sick bus, so re-read the clock instead of
				 * pacing the remaining slots off a stale one.
				 */
				now = k_uptime_get();
				sensor_slot_reschedule(slot, now, ok);
			}

			if (slot->due_ms < next_due) {
				next_due = slot->due_ms;
			}
		}

		if (next_due > now) {
			/* Absolute deadline */
			k_sleep(K_TIMEOUT_ABS_MS(next_due));
		} else {
			k_yield();
		}
	}
#endif /* AURORA_HAVE_POLLED_SENSOR */
}

K_THREAD_DEFINE(sensor_polling, 4096, sensor_task, NULL, NULL, NULL, 7, 0, 0);
#endif /* (CONFIG_IMU || CONFIG_BARO) && !CONFIG_AURORA_FAKE_SENSORS */

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
#define SM_DRAIN_LIMIT 32

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
 * system is in the IDLE state, and computes vertical acceleration once calibrated.
 *
 * @param[in,out] last_imu_ns         Pointer to the timestamp of the last processed IMU sample.
 * @param[in,out] attitude_state      Pointer to the internal attitude estimation filter state.
 * @param[in]     imu_data            Pointer to the incoming raw IMU message buffer.
 * @param[out]    orientation         Array where the calculated orientation [pitch, roll, yaw] is stored.
 * @param[out]    acceleration        Pointer where the scalar acceleration magnitude is stored.
 * @param[out]    accel_vert          Pointer where the earth-frame vertical acceleration is stored.
 * @param[out]    imu_ready           Pointer set to true once fresh orientation and acceleration are available.
 * @param[in,out] calibration_started_notified Pointer tracking whether the user has been notified of the start.
 * @param[in,out] calibration_notified Pointer tracking whether the user has been notified of completion.
 */
static void handle_imu(int64_t *last_imu_ns, struct attitude *attitude_state, struct imu_data *imu_data,
	double orientation[3], double *acceleration, double *accel_vert, bool *imu_ready,
	bool *calibration_started_notified, bool *calibration_notified)
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
		if (sm_get_state() == SM_IDLE) {
			attitude_calibrate_sample(attitude_state, accel_b, gyro_b);
#if defined(CONFIG_AURORA_NOTIFY)
			if (!(*calibration_started_notified) &&
			    attitude_state->cal_samples > 0) {
				notify_calibration_start();
				*calibration_started_notified = true;
			}
#endif /* CONFIG_AURORA_NOTIFY */
			if (attitude_calibrate_converged(attitude_state)) {
				if (attitude_calibrate_finish(attitude_state) == 0) {
#if defined(CONFIG_AURORA_STATE_MACHINE_RETAIN)
					/* Snapshot it now, while the vehicle is
					 * still stationary and the result is
					 * known good.  Calibration only runs in
					 * SM_IDLE, so a board that reboots into
					 * a flight state can never redo this --
					 * it has to be handed the old answer.
					 */
					(void)sm_retain_save_blob(attitude_state,
								  sizeof(*attitude_state));
#endif /* CONFIG_AURORA_STATE_MACHINE_RETAIN */
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

static void handle_state_transition(enum sm_state prev_state, enum sm_state state, struct attitude *attitude_state,
	int64_t *last_imu_ns, bool *calibration_started_notified, bool *calibration_notified,
	double orientation[3])
{
#if defined(CONFIG_AURORA_FAKE_SENSORS) && defined(CONFIG_AURORA_SIM_AUTOTEST)
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
		*calibration_started_notified = false;
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
	/* No-IMU builds have nothing to calibrate, so treat calibration as
	 * instantly satisfied; CONFIG_IMU builds override this below from
	 * the real attitude tracker each iteration.
	 */
	bool calibrated = true;

	struct sm_error_handling_args sm_error_handler = {
		.cb = &state_machine_error_handler,
		.args = NULL,
	};

	struct sm_inputs inputs = {
		.armed = 1,
		.acceleration = acceleration,
		.accel_vert = accel_vert,
	};
	memcpy(inputs.orientation, orientation, sizeof(inputs.orientation));
#if defined(CONFIG_IMU)
	static struct attitude attitude_state;
	int64_t last_imu_ns = 0;
	bool calibration_started_notified = false;
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

	/* Per-vehicle thresholds with Kconfig fallbacks */
	sm_config_load(&state_cfg);

	sm_init(&state_cfg, &sm_error_handler);
	sm_active = true;

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

#if defined(CONFIG_AURORA_STATE_MACHINE_RETAIN) && defined(CONFIG_AURORA_NOTIFY)
	/* Announce retain as an IDLE => resumed edge */
	if (sm_retain_recovered()) {
		enum sm_state resumed = sm_get_state();

		if (resumed != prev_state) {
			LOG_WRN("resumed in %s after reset; re-syncing notifications",
				sm_state_str(resumed));
			notify_state_change(prev_state, resumed);
			prev_state = resumed;
		}
	}
#endif /* CONFIG_AURORA_STATE_MACHINE_RETAIN && CONFIG_AURORA_NOTIFY */

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
		unsigned int drained = 0;

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
					&calibration_started_notified,
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
		} while (++drained < SM_DRAIN_LIMIT &&
			 zbus_sub_wait_msg(&sm_sub, &data_chan, &msg_buf, K_NO_WAIT) == 0);

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

#if defined(CONFIG_IMU)
		calibrated = attitude_is_calibrated(&attitude_state) > 0;
#endif /* CONFIG_IMU */

		inputs = (struct sm_inputs){
			.armed = 1,
			.log_ready = log_flight_log_online(),
			.calibrated = calibrated,
			.acceleration = acceleration,
			.accel_vert = accel_vert,
			.altitude = altitude,
		};
		memcpy(inputs.orientation, orientation, sizeof(inputs.orientation));

		sm_update(&inputs);
		state = sm_get_state();

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
				&calibration_started_notified,
				&calibration_notified,
				orientation);
#else
			handle_state_transition(prev_state, state, NULL, NULL, NULL, NULL, orientation);
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
