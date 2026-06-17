#include "logger.h"
#include <aurora/lib/baro.h>
#include <stdint.h>
#include <stdio.h>
#include "aurora/lib/data_logger.h"
#include "aurora/lib/notify.h"
#include "aurora/lib/state/state.h"
#include "zephyr/fs/fs.h"
#include "zephyr/logging/log.h"

LOG_MODULE_DECLARE(main, CONFIG_SENSOR_BOARD_LOG_LEVEL);

K_THREAD_DEFINE(logger_thread, 2048, logger_task, NULL, NULL, NULL, 8, 0, 0);
K_THREAD_DEFINE(converter_thread, 2048, converter_task, NULL, NULL, NULL, 9, 0, 0);

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
	if (data_logger_init(&sm_logger, "flight", &data_logger_bin_formatter) != 0) {
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

void log_flight_telemetry()
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
