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

static struct data_logger sm_logger;
static atomic_t sm_logger_live = ATOMIC_INIT(0);

/* Decouple logging from the SM hot path: SM pushes datapoints into this
 * queue with K_NO_WAIT; a dedicated logger thread drains it and owns all
 * FS-touching operations (write + periodic flush). Sized to absorb the
 * worst-case flush stall at full IMU+baro rate.
 */
#define LOG_MSGQ_DEPTH 256
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

K_THREAD_DEFINE(logger_thread, 2048, logger_task, NULL, NULL, NULL, 8, 0, 0);

/* Pick the next free /<base>/flight_<N>.<probe_ext> on the filesystem,
 * writing "/<base>/flight_<N>" (without extension) into @p out. The
 * converter then appends each formatter's file_ext. Falls back to
 * index 0 (overwrites) if every slot is taken.
 */
static void pick_convert_out_base(char *out, size_t out_sz)
{
#if defined(CONFIG_DATA_LOGGER_CONVERT_CSV)
	const char *probe_ext = data_logger_csv_formatter.file_ext;
#elif defined(CONFIG_DATA_LOGGER_CONVERT_INFLUX)
	const char *probe_ext = data_logger_influx_formatter.file_ext;
#else
	const char *probe_ext = "out";
#endif

	for (int i = 0; i <= CONFIG_DATA_LOGGER_MAX_FILES; i++) {
		char probe[DATA_LOGGER_PATH_MAX];
		struct fs_dirent entry;

		int n = snprintf(probe, sizeof(probe), "%s/flight_%d.%s", CONFIG_DATA_LOGGER_BASE_PATH, i,
						 probe_ext);
		if (n < 0 || n >= (int)sizeof(probe)) {
			break;
		}
		if (fs_stat(probe, &entry) == -ENOENT) {
			(void)snprintf(out, out_sz, "%s/flight_%d", CONFIG_DATA_LOGGER_BASE_PATH, i);
			return;
		}
	}

	LOG_WRN("convert: out of /flight_N slots — overwriting flight_0");
	(void)snprintf(out, out_sz, "%s/flight_0", CONFIG_DATA_LOGGER_BASE_PATH);
}

static void converter_task(void *, void *, void *)
{
	while (1) {
		k_sem_take(&convert_request, K_FOREVER);
		k_sem_take(&convert_idle, K_FOREVER);

		/* DATA_LOGGER_PATH_MAX minus struct data_logger_formatter's member
		 * "file_ext" */
		char base[DATA_LOGGER_PATH_MAX - 8];

		pick_convert_out_base(base, sizeof(base));

#if defined(CONFIG_DATA_LOGGER_CONVERT_CSV)
		{
			char out_path[DATA_LOGGER_PATH_MAX];

			(void)snprintf(out_path, sizeof(out_path), "%s.%s", base,
						   data_logger_csv_formatter.file_ext);
			int rc = data_logger_convert(&data_logger_csv_formatter, out_path);

			if (rc != 0) {
				LOG_ERR("CSV conversion → %s failed (%d)", out_path, rc);
			} else {
				LOG_INF("converted flight_log → %s", out_path);
			}
		}
#endif /* CONFIG_DATA_LOGGER_CONVERT_CSV */

#if defined(CONFIG_DATA_LOGGER_CONVERT_INFLUX)
		{
			char out_path[DATA_LOGGER_PATH_MAX];

			(void)snprintf(out_path, sizeof(out_path), "%s.%s", base,
						   data_logger_influx_formatter.file_ext);
			int rc = data_logger_convert(&data_logger_influx_formatter, out_path);

			if (rc != 0) {
				LOG_ERR("Influx conversion → %s failed (%d)", out_path, rc);
			} else {
				LOG_INF("converted flight_log → %s", out_path);
			}
		}
#endif /* CONFIG_DATA_LOGGER_CONVERT_INFLUX */

		k_sem_give(&convert_idle);
	}
}

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

void log_imu_data(const struct imu_data *imu)
{
	uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
	struct datapoint dp = {
		.timestamp_ns = ts,
		.type = AURORA_DATA_IMU_ACCEL,
		.channel_count = 3,
		.channels = {imu->accel[0], imu->accel[1], imu->accel[2]},
	};
	log_enqueue(&dp);

	dp.type = AURORA_DATA_IMU_GYRO;
	dp.channels[0] = imu->gyro[0];
	dp.channels[1] = imu->gyro[1];
	dp.channels[2] = imu->gyro[2];
	log_enqueue(&dp);
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

void log_baro_data(const struct baro_data *baro)
{
	uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
	struct datapoint dp = {
		.timestamp_ns = ts,
		.type = AURORA_DATA_BARO,
		.channel_count = 2,
		.channels = {baro->temperature, baro->pressure},
	};
	log_enqueue(&dp);
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
