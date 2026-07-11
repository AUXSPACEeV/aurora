/**
 * @file data.c
 * @brief Flight-time logging lifecycle and telemetry glue for the sensor board.
 *
 * Drives the binary data logger across flight state transitions and enqueues
 * state-machine telemetry datapoints.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "data.h"
#include <stdint.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <aurora/lib/baro.h>
#include <aurora/lib/data_logger.h>
#include <aurora/lib/notify.h>
#include <aurora/lib/state/state.h>

#if defined(CONFIG_AURORA_PAD_LINK)
#include <aurora/lib/pad_link.h>
#endif /* CONFIG_AURORA_PAD_LINK */

#if defined(CONFIG_DATA_LOGGER_BIN)
LOG_MODULE_DECLARE(main, CONFIG_SENSOR_BOARD_LOG_LEVEL);

K_THREAD_DEFINE(logger_thread, 2048, logger_task, NULL, NULL, NULL, 8, 0, 0);
K_THREAD_DEFINE(converter_thread, 2048, converter_task, NULL, NULL, NULL, 9, 0, 0);

/* Latched (until reboot) when opening the flight log at ARM time fails.
 * Feeds log_flight_log_online() so the state machine drops back from ARMED
 * to IDLE instead of sitting armed without a recording.  Deliberately never
 * cleared: clearing it on disarm would let the unchanged inputs re-arm
 * immediately and oscillate the pyro channels.
 */
static atomic_t flight_recording_failed = ATOMIC_INIT(0);

static void log_begin_flight(void)
{
	if (k_sem_take(&convert_idle, K_MSEC(LOG_ARM_CONVERT_TIMEOUT_MS)) != 0) {
		LOG_ERR("ARMED: prior conversion still running after %d ms — "
				"flight will not be logged",
			LOG_ARM_CONVERT_TIMEOUT_MS);
		atomic_set(&flight_recording_failed, 1);
#if defined(CONFIG_AURORA_NOTIFY)
		notify_error();
#endif
		return;
	}
	k_sem_give(&convert_idle);

	memset(&sm_logger, 0, sizeof(sm_logger));
	if (data_logger_init(&sm_logger, "flight", &data_logger_bin_formatter) != 0) {
		LOG_ERR("data_logger_init failed");
		atomic_set(&flight_recording_failed, 1);
#if defined(CONFIG_AURORA_NOTIFY)
		notify_error();
#endif
		return;
	}
	data_logger_set_default(&sm_logger);
	if (data_logger_start(&sm_logger) != 0) {
		LOG_ERR("data_logger_start failed");
		atomic_set(&flight_recording_failed, 1);
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
	(void)data_logger_close(&sm_logger);
	data_logger_set_default(NULL);

	k_sem_give(&convert_request);
}

/* Delayable close: scheduled by the state-machine task on entry to
 * LANDED so the post-landed pad window gets logged before the logger is
 * closed and conversion kicks in.
 */
static void log_end_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	log_end_flight();
}
static K_WORK_DELAYABLE_DEFINE(log_end_work, log_end_work_handler);
bool log_flight_log_online(void)
{
	/* An ARM-time open failure outranks the boot latch: the disk looked
	 * fine at boot but the recorder could not be opened when it mattered.
	 */
	if (atomic_get(&flight_recording_failed)) {
		return false;
	}
#if defined(CONFIG_DATA_LOGGER_DISK_AUTO_MKFS)
	return flight_log_online();
#else
	/* No disk bring-up module.  Nothing to gate on at boot. */
	return true;
#endif
}

void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state)
{
	/* Flight-time logging lifecycle:
	 *  - IDLE→ARMED:  open a fresh binary log (cancel any
	 *                 still-pending deferred close from a
	 *                 previous flight first).
	 *  - ARMED→BOOST: hand the formatter a BOOST event so
	 *                 the circular ring freezes forward.
	 *  - →LANDED:     hand a LANDED event and schedule the
	 *                 close POST_LANDED_PAD_MS later so the
	 *                 tail of the flight gets captured.
	 *  - →IDLE/ERROR: close immediately (no pad); cancel
	 *                 any pending deferred close.
	 */
	if (prev_state == SM_IDLE && state == SM_ARMED) {
		(void)k_work_cancel_delayable(&log_end_work);
		log_begin_flight();
	} else if (prev_state == SM_ARMED && state == SM_BOOST) {
		(void)data_logger_event(&sm_logger, DLE_BOOST);
	} else if (state == SM_LANDED) {
		(void)data_logger_event(&sm_logger, DLE_LANDED);
		(void)k_work_schedule(&log_end_work, K_MSEC(CONFIG_DATA_LOGGER_BIN_POST_LANDED_PAD_MS));
	} else if (state == SM_IDLE || state == SM_ERROR) {
		(void)k_work_cancel_delayable(&log_end_work);
		log_end_flight();
	}
}

void log_flight_telemetry(void)
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

#if DT_HAS_CHOSEN(auxspace_vbat)
/* Battery voltage changes slowly; cap the ADC sample rate independently of
 * the (much faster) state-machine telemetry cadence.
 */
#define LOG_VBAT_PERIOD_MS 500

static const struct device *const vbat_dev = DEVICE_DT_GET(DT_CHOSEN(auxspace_vbat));

void log_vbat_telemetry(void)
{
	static int64_t next_sample_ms;

	int64_t now = k_uptime_get();
	if (now < next_sample_ms) {
		return;
	}
	next_sample_ms = now + LOG_VBAT_PERIOD_MS;

	if (!device_is_ready(vbat_dev)) {
		return;
	}

	struct sensor_value voltage;
	if (sensor_sample_fetch(vbat_dev) != 0 ||
	    sensor_channel_get(vbat_dev, SENSOR_CHAN_VOLTAGE, &voltage) != 0) {
		LOG_WRN("vbat read failed");
		return;
	}

	struct datapoint dp = {
		.timestamp_ns = k_ticks_to_ns_floor64(k_uptime_ticks()),
		.type = AURORA_DATA_VBAT,
		.channel_count = 1,
		.channels = {voltage},
	};
	log_enqueue(&dp);
}
#else
/* No battery-sense node in the devicetree — nothing to log. */
void log_vbat_telemetry(void) {}
#endif /* DT_HAS_CHOSEN(auxspace_vbat) */
#endif

#if defined(CONFIG_AURORA_PAD_LINK)

void update_pad_link_data(void)
{
	struct sm_inputs sm_snap;

	sm_get_inputs(&sm_snap);
	pad_link_publish_sm(sm_get_state(), sm_get_type(), &sm_snap);
}
#endif /* CONFIG_AURORA_PAD_LINK */
