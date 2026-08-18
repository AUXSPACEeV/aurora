/**
 * @file fake_sensors_replay.c
 * @brief Real-flight-data replay backend for fake_sensors.
 *
 * When CONFIG_AURORA_FAKE_SENSORS_REPLAY is enabled, replaces the
 * synthetic flight-profile generator (see fake_sensors.c) with a
 * playback engine that answers a fetch with the accelerometer,
 * gyroscope and barometer sample a real recorded flight held at that
 * instant, published onto the same zbus channels as the live sensor
 * drivers. Sample data is generated at build time by
 * aurora/tools/gen_flight_replay.py from a flights.csv produced by the
 * data_logger CSV converter; the resulting arrays land in replay_data.c.
 *
 * Behaviour mirrors fake_sensors.c: it supplies no threads of its own,
 * and sits pad-stationary (answering with the first recorded sample)
 * until `sim launch` fires, at which point the recording plays back
 * against its original timeline -- each fetch returns the newest sample
 * whose timestamp has already passed, so main.c's sampling rate decides
 * the resolution the recording is seen at. `sim reset` returns to
 * pad-stationary. With CONFIG_AURORA_SIM_AUTOTEST=y the autolaunch
 * thread fires the launch automatically once attitude calibration
 * completes and exits the simulator on SM_LANDED / SM_ERROR.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_sensors.h"
#include "fake_sensors_replay.h"
#include "aurora/lib/state/state.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/baro.h>
#include <aurora/lib/imu.h>

LOG_MODULE_REGISTER(fake_sensors_replay, CONFIG_SENSOR_BOARD_LOG_LEVEL);

/* -------- Calibration signal (written by main.c, read by autolaunch) -------- */

static struct k_spinlock cal_lock;
static bool calibration_done;

/* fake_sensors_on_calibrated - see fake_sensors.h */
void fake_sensors_on_calibrated(void)
{
	k_spinlock_key_t key = k_spin_lock(&cal_lock);
	calibration_done = true;
	k_spin_unlock(&cal_lock, key);
}

/* -------- Launch trigger state (shared with shell command) -------- */

static struct k_spinlock launch_lock;
/** Uptime (ns) at which `sim launch` fired. 0 means pad-stationary. */
static uint64_t launch_uptime_ns;

static uint64_t launch_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&launch_lock);
	uint64_t v = launch_uptime_ns;
	k_spin_unlock(&launch_lock, key);
	return v;
}

static void launch_set(uint64_t v)
{
	k_spinlock_key_t key = k_spin_lock(&launch_lock);
	launch_uptime_ns = v;
	k_spin_unlock(&launch_lock, key);
}

/* -------- Helpers -------- */

static void set_sensor_value_double(struct sensor_value *sv, double v)
{
	sv->val1 = (int32_t)v;
	sv->val2 = (int32_t)((v - (double)sv->val1) * 1000000.0);
}

static int publish_imu(const struct replay_imu_sample *a,
		       const struct replay_imu_sample *g)
{
	struct imu_data msg = {0};
	set_sensor_value_double(&msg.accel[0], a->x);
	set_sensor_value_double(&msg.accel[1], a->y);
	set_sensor_value_double(&msg.accel[2], a->z);
	set_sensor_value_double(&msg.gyro[0], g->x);
	set_sensor_value_double(&msg.gyro[1], g->y);
	set_sensor_value_double(&msg.gyro[2], g->z);
	return zbus_chan_pub(&imu_data_chan, &msg, K_NO_WAIT);
}

static int publish_baro(const struct replay_baro_sample *b)
{
	struct baro_data msg = {0};
	set_sensor_value_double(&msg.temperature, b->temp_c);
	set_sensor_value_double(&msg.pressure, b->pres_kpa);
	return zbus_chan_pub(&baro_data_chan, &msg, K_NO_WAIT);
}

/* -------- Playback cursors -------- */

/**
 * @brief Where playback of one sensor's tracks has got to.
 *
 * The indices only ever move forward within a run; `sim launch` and
 * `sim reset` are seen as a change of @c origin, which rewinds them.
 */
struct replay_cursor {
	uint64_t origin;  /**< Launch instant the indices belong to; 0 = pad. */
	size_t idx;       /**< Cursor into the primary track. */
	size_t idx2;      /**< Cursor into the secondary track (IMU gyro). */
	bool end_logged;  /**< End of recording already announced. */
};

static struct replay_cursor imu_cursor;
static struct replay_cursor baro_cursor;

/**
 * @brief Resolve the recording time this fetch should sample at.
 *
 * Rewinds @p cur first if `sim launch` or `sim reset` moved the origin
 * since the last fetch.
 *
 * @param cur  Cursor for the sensor being fetched.
 * @param t_ns Set to the elapsed recording time when playback is running.
 *
 * @retval true  Playback is running; @p t_ns is valid.
 * @retval false Pad-stationary; the caller holds the first recorded sample.
 */
static bool replay_time_ns(struct replay_cursor *cur, uint64_t *t_ns)
{
	const uint64_t origin = launch_get();

	if (origin != cur->origin) {
		cur->origin = origin;
		cur->idx = 0;
		cur->idx2 = 0;
		cur->end_logged = false;
	}

	if (origin == 0) {
		return false;
	}

	const uint64_t now = k_ticks_to_ns_floor64(k_uptime_ticks());

	*t_ns = (now > origin) ? (now - origin) : 0;
	return true;
}

/**
 * @brief Advance a cursor to the newest sample at or before @p t_ns.
 *
 * Reading a sensor returns whatever it holds at that instant, so samples
 * the caller sampled past are skipped rather than queued: how much of the
 * recording is seen is a function of the rate main.c fetches at.
 */
static size_t replay_seek_imu(size_t idx, uint64_t t_ns,
			      const struct replay_imu_sample *track, size_t len)
{
	while (idx + 1 < len && track[idx + 1].t_ns <= t_ns) {
		idx++;
	}
	return idx;
}

static size_t replay_seek_baro(size_t idx, uint64_t t_ns,
			       const struct replay_baro_sample *track, size_t len)
{
	while (idx + 1 < len && track[idx + 1].t_ns <= t_ns) {
		idx++;
	}
	return idx;
}

/* -------- Replay IMU interface -------- */

/* fake_imu_init - see fake_sensors.h */
int fake_imu_init(void)
{
	if (replay_accel_len == 0 || replay_gyro_len == 0) {
		LOG_ERR("Replay IMU: recording holds no samples");
		return -ENODATA;
	}

	LOG_INF("Replay IMU: %zu accel + %zu gyro samples "
		"(pad-stationary, awaiting `sim launch`)",
		replay_accel_len, replay_gyro_len);
	return 0;
}

/* fake_imu_poll - see fake_sensors.h */
int fake_imu_poll(const struct device *dev)
{
	ARG_UNUSED(dev);

	uint64_t t_ns;

	/* Pad-stationary: hold the first recorded sample so attitude
	 * calibration sees a valid stationary signal.
	 */
	if (!replay_time_ns(&imu_cursor, &t_ns)) {
		return publish_imu(&replay_accel[0], &replay_gyro[0]);
	}

	imu_cursor.idx = replay_seek_imu(imu_cursor.idx, t_ns,
					 replay_accel, replay_accel_len);
	imu_cursor.idx2 = replay_seek_imu(imu_cursor.idx2, t_ns,
					  replay_gyro, replay_gyro_len);

	if (!imu_cursor.end_logged &&
	    t_ns > replay_accel[replay_accel_len - 1].t_ns) {
		LOG_INF("Replay IMU: end of recording, holding final sample");
		imu_cursor.end_logged = true;
	}

	return publish_imu(&replay_accel[imu_cursor.idx],
			   &replay_gyro[imu_cursor.idx2]);
}

/* -------- Replay baro interface -------- */

/* fake_baro_init - see fake_sensors.h */
int fake_baro_init(void)
{
	if (replay_baro_len == 0) {
		LOG_ERR("Replay baro: recording holds no samples");
		return -ENODATA;
	}

	LOG_INF("Replay baro: %zu samples (pad-stationary, awaiting `sim launch`)",
		replay_baro_len);
	return 0;
}

/* fake_baro_measure - see fake_sensors.h */
int fake_baro_measure(const struct device *dev)
{
	ARG_UNUSED(dev);

	uint64_t t_ns;

	if (!replay_time_ns(&baro_cursor, &t_ns)) {
		return publish_baro(&replay_baro[0]);
	}

	baro_cursor.idx = replay_seek_baro(baro_cursor.idx, t_ns,
					   replay_baro, replay_baro_len);

	if (!baro_cursor.end_logged &&
	    t_ns > replay_baro[replay_baro_len - 1].t_ns) {
		LOG_INF("Replay baro: end of recording, holding final sample");
		baro_cursor.end_logged = true;
	}

	return publish_baro(&replay_baro[baro_cursor.idx]);
}

/* -------- Shell interface -------- */

static int cmd_sim_launch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	launch_set(k_ticks_to_ns_floor64(k_uptime_ticks()));
	shell_print(sh, "sim: replay started (%zu accel / %zu gyro / %zu baro samples)",
		    replay_accel_len, replay_gyro_len, replay_baro_len);
	return 0;
}

static int cmd_sim_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	launch_set(0);
	shell_print(sh, "sim: replay reset (pad-stationary)");
	return 0;
}

static int cmd_sim_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint64_t origin = launch_get();
	if (origin == 0) {
		shell_print(sh, "sim: pad-stationary");
	} else {
		uint64_t now = k_ticks_to_ns_floor64(k_uptime_ticks());
		double t_s = (double)(now - origin) / 1e9;
		double total_s = (double)replay_accel[replay_accel_len - 1].t_ns / 1e9;
		shell_print(sh, "sim: replay t=%.2fs / %.2fs", t_s, total_s);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sim_subcmds,
	SHELL_CMD(launch, NULL,
		  "Start replay of the embedded flight recording",
		  cmd_sim_launch),
	SHELL_CMD(reset, NULL,
		  "Reset replay back to pad-stationary",
		  cmd_sim_reset),
	SHELL_CMD(status, NULL,
		  "Print current replay state",
		  cmd_sim_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sim, &sim_subcmds,
		   "Recorded-flight replay simulator", NULL);

#if defined(CONFIG_AURORA_SIM_AUTOTEST)
/**
 * @brief Wait for attitude calibration to finish, fire `sim launch`
 *        automatically, then exit the simulator on LANDED/ERROR.
 */
static void autolaunch_task(void *, void *, void *)
{
	LOG_INF("replay autolaunch: waiting for attitude calibration...");
	bool done = false;
	while (!done) {
		k_spinlock_key_t key = k_spin_lock(&cal_lock);
		done = calibration_done;
		k_spin_unlock(&cal_lock, key);
		if (done) {
			break;
		}
		k_msleep(50);
	}

	LOG_INF("replay autolaunch: launching replay");
	launch_set(k_ticks_to_ns_floor64(k_uptime_ticks()));

	int64_t deadline = k_uptime_get() + CONFIG_AURORA_SIM_AUTOLAUNCH_TIMEOUT_MS;

	while (1) {
		enum sm_state s = sm_get_state();

		if (s == SM_LANDED) {
			struct sm_inputs sm_in;
			sm_get_inputs(&sm_in);
			LOG_INF("replay autolaunch: orientation yaw=%.2f pitch=%.2f roll=%.2f",
				sm_in.orientation[0], sm_in.orientation[1],
				sm_in.orientation[2]);
			LOG_INF("replay autolaunch: LANDED - simulation complete");
			log_flush();
			exit(0);
		}
		if (s == SM_ERROR) {
			LOG_ERR("replay autolaunch: ERROR state - simulation failed");
			log_flush();
			exit(1);
		}
		if (k_uptime_get() >= deadline) {
			LOG_ERR("replay autolaunch: timeout after %d ms without landing",
				CONFIG_AURORA_SIM_AUTOLAUNCH_TIMEOUT_MS);
			log_flush();
			exit(1);
		}
		k_msleep(100);
	}
}

K_THREAD_DEFINE(autolaunch, 1024, autolaunch_task, NULL, NULL, NULL, 5, 0, 0);
#endif /* CONFIG_AURORA_SIM_AUTOTEST */
