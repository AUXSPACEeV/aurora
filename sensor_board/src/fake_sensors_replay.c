/**
 * @file fake_sensors_replay.c
 * @brief Real-flight-data replay backend for fake_sensors.
 *
 * When CONFIG_AURORA_FAKE_SENSORS_REPLAY is enabled, replaces the
 * synthetic flight-profile generator (see fake_sensors.c) with a
 * playback engine that streams accelerometer, gyroscope and barometer
 * samples taken from a real recorded flight onto the same zbus
 * channels as the live sensor drivers. Sample data is generated at
 * build time by aurora/tools/gen_flight_replay.py from a flights.csv
 * produced by the data_logger CSV converter; the resulting arrays land
 * in replay_data.c.
 *
 * Behaviour mirrors fake_sensors.c: the threads sit pad-stationary
 * (republishing the first recorded sample at the configured IMU/baro
 * cadence) until `sim launch` fires, at which point the recording is
 * replayed from t=0 at its original timeline. `sim reset` returns to
 * pad-stationary. With CONFIG_AURORA_SIM_AUTOTEST=y the autolaunch
 * thread fires the launch automatically once attitude calibration
 * completes and exits the simulator on SM_LANDED / SM_ERROR.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

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

extern bool baro_active;
extern bool imu_active;

/* -------- Calibration signal (written by main.c, read by autolaunch) -------- */

static struct k_spinlock cal_lock;
static bool calibration_done;

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

static void publish_imu(const struct replay_imu_sample *a,
			const struct replay_imu_sample *g)
{
	struct imu_data msg = {0};
	set_sensor_value_double(&msg.accel[0], a->x);
	set_sensor_value_double(&msg.accel[1], a->y);
	set_sensor_value_double(&msg.accel[2], a->z);
	set_sensor_value_double(&msg.gyro[0], g->x);
	set_sensor_value_double(&msg.gyro[1], g->y);
	set_sensor_value_double(&msg.gyro[2], g->z);
	(void)zbus_chan_pub(&imu_data_chan, &msg, K_NO_WAIT);
}

static void publish_baro(const struct replay_baro_sample *b)
{
	struct baro_data msg = {0};
	set_sensor_value_double(&msg.temperature, b->temp_c);
	set_sensor_value_double(&msg.pressure, b->pres_kpa);
	(void)zbus_chan_pub(&baro_data_chan, &msg, K_NO_WAIT);
}

static void sleep_until(uint64_t target_ns)
{
	uint64_t now = k_ticks_to_ns_floor64(k_uptime_ticks());
	if (target_ns > now) {
		k_sleep(K_NSEC(target_ns - now));
	}
}

/* -------- Replay IMU thread -------- */

static void replay_imu_task(void *, void *, void *)
{
	const int hz = CONFIG_IMU_FREQUENCY;
	const uint64_t period_ns = 1000000000ULL / hz;

	LOG_INF("Replay IMU: %zu accel + %zu gyro samples (pad-stationary, awaiting `sim launch`)",
		replay_accel_len, replay_gyro_len);
	imu_active = true;

	while (1) {
		uint64_t origin = launch_get();
		if (origin == 0) {
			/* Pad-stationary: republish the first recorded
			 * sample at the configured cadence so attitude
			 * calibration sees a valid stationary signal.
			 */
			publish_imu(&replay_accel[0], &replay_gyro[0]);
			k_sleep(K_NSEC(period_ns));
			continue;
		}

		size_t gi = 0;
		for (size_t ai = 0; ai < replay_accel_len; ai++) {
			if (launch_get() != origin) {
				break; /* sim reset / re-launch */
			}
			const struct replay_imu_sample *a = &replay_accel[ai];

			while (gi + 1 < replay_gyro_len &&
			       replay_gyro[gi + 1].t_ns <= a->t_ns) {
				gi++;
			}
			sleep_until(origin + a->t_ns);
			publish_imu(a, &replay_gyro[gi]);
		}

		if (launch_get() == origin) {
			LOG_INF("Replay IMU: end of recording, holding final sample");
			while (launch_get() == origin) {
				publish_imu(&replay_accel[replay_accel_len - 1],
					    &replay_gyro[replay_gyro_len - 1]);
				k_sleep(K_NSEC(period_ns));
			}
		}
	}
}

K_THREAD_DEFINE(imu_polling, 2048, replay_imu_task, NULL, NULL, NULL,
		5, 0, 0);

/* -------- Replay baro thread -------- */

static void replay_baro_task(void *, void *, void *)
{
	const int hz = CONFIG_BARO_FREQUENCY;
	const uint64_t period_ns = 1000000000ULL / hz;

	LOG_INF("Replay baro: %zu samples (pad-stationary, awaiting `sim launch`)",
		replay_baro_len);
	baro_active = true;

	while (1) {
		uint64_t origin = launch_get();
		if (origin == 0) {
			publish_baro(&replay_baro[0]);
			k_sleep(K_NSEC(period_ns));
			continue;
		}

		for (size_t i = 0; i < replay_baro_len; i++) {
			if (launch_get() != origin) {
				break;
			}
			const struct replay_baro_sample *b = &replay_baro[i];
			sleep_until(origin + b->t_ns);
			publish_baro(b);
		}

		if (launch_get() == origin) {
			LOG_INF("Replay baro: end of recording, holding final sample");
			while (launch_get() == origin) {
				publish_baro(&replay_baro[replay_baro_len - 1]);
				k_sleep(K_NSEC(period_ns));
			}
		}
	}
}

K_THREAD_DEFINE(baro_polling, 2048, replay_baro_task, NULL, NULL, NULL,
		5, 0, 0);

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
