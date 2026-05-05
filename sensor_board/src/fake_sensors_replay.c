/**
 * @file fake_sensors_replay.c
 * @brief Real-flight-data replay backend for fake_sensors.
 *
 * When CONFIG_AURORA_FAKE_SENSORS_REPLAY is enabled, replaces the
 * synthetic flight-profile generator (see fake_sensors.c) with a
 * playback engine that streams accelerometer, gyroscope and barometer
 * samples taken from a real recorded flight onto the same zbus channels
 * as the live sensor drivers. Sample data is generated at build time by
 * aurora/scripts/gen_flight_replay.py from a flights.csv produced by
 * the data_logger CSV converter; the resulting arrays land in
 * replay_data.c.
 *
 * Playback starts at boot at wall-clock 1:1 with the recording's
 * relative timestamps, so the pre-launch portion of the recording
 * naturally feeds attitude calibration and the SM picks up BOOST,
 * APOGEE, MAIN and LANDED at the same offsets the original flight saw
 * them.
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
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/baro.h>
#include <aurora/lib/imu.h>

LOG_MODULE_REGISTER(fake_sensors_replay, CONFIG_SENSOR_BOARD_LOG_LEVEL);

extern bool baro_active;
extern bool imu_active;

/* Wall-clock origin: set once at boot, then sample t_ns is added to it
 * to compute the absolute uptime each sample is due.
 */
static uint64_t replay_origin_ns;

/* No-op stub: main.c calls this on calibration completion when fake
 * sensors are enabled. The synth backend uses it to gate launch; the
 * replay backend's "launch" is encoded in the recording itself.
 */
void fake_sensors_on_calibrated(void)
{
}

static void set_sensor_value_double(struct sensor_value *sv, double v)
{
	sv->val1 = (int32_t)v;
	sv->val2 = (int32_t)((v - (double)sv->val1) * 1000000.0);
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
	LOG_INF("Replay IMU: %zu accel + %zu gyro samples",
		replay_accel_len, replay_gyro_len);
	imu_active = true;

	/* Walk the accel and gyro streams together, emitting one
	 * imu_data message per accel sample with the latest gyro reading
	 * merged in. The recording timestamps the two streams identically
	 * in practice, but we don't rely on that.
	 */
	size_t gi = 0;
	for (size_t ai = 0; ai < replay_accel_len; ai++) {
		const struct replay_imu_sample *a = &replay_accel[ai];

		while (gi + 1 < replay_gyro_len &&
		       replay_gyro[gi + 1].t_ns <= a->t_ns) {
			gi++;
		}
		const struct replay_imu_sample *g = &replay_gyro[gi];

		sleep_until(replay_origin_ns + a->t_ns);

		struct imu_data msg = {0};
		set_sensor_value_double(&msg.accel[0], a->x);
		set_sensor_value_double(&msg.accel[1], a->y);
		set_sensor_value_double(&msg.accel[2], a->z);
		set_sensor_value_double(&msg.gyro[0], g->x);
		set_sensor_value_double(&msg.gyro[1], g->y);
		set_sensor_value_double(&msg.gyro[2], g->z);

		(void)zbus_chan_pub(&imu_data_chan, &msg, K_NO_WAIT);
	}

	LOG_INF("Replay IMU: end of recording, parking thread");
	while (1) {
		k_sleep(K_FOREVER);
	}
}

K_THREAD_DEFINE(imu_polling, 2048, replay_imu_task, NULL, NULL, NULL,
		5, 0, 0);

/* -------- Replay baro thread -------- */

static void replay_baro_task(void *, void *, void *)
{
	LOG_INF("Replay baro: %zu samples", replay_baro_len);
	baro_active = true;

	for (size_t i = 0; i < replay_baro_len; i++) {
		const struct replay_baro_sample *b = &replay_baro[i];

		sleep_until(replay_origin_ns + b->t_ns);

		struct baro_data msg = {0};
		set_sensor_value_double(&msg.temperature, b->temp_c);
		set_sensor_value_double(&msg.pressure, b->pres_kpa);
		(void)zbus_chan_pub(&baro_data_chan, &msg, K_NO_WAIT);
	}

	LOG_INF("Replay baro: end of recording, parking thread");
	while (1) {
		k_sleep(K_FOREVER);
	}
}

K_THREAD_DEFINE(baro_polling, 2048, replay_baro_task, NULL, NULL, NULL,
		5, 0, 0);

/* -------- Origin initializer -------- */

static int replay_init(void)
{
	replay_origin_ns = k_ticks_to_ns_floor64(k_uptime_ticks());
	return 0;
}
SYS_INIT(replay_init, APPLICATION, 0);

#if defined(CONFIG_AURORA_SIM_AUTOTEST)
/**
 * @brief Watch for SM_LANDED/SM_ERROR and exit the simulator accordingly.
 *
 * Mirrors the autolaunch task in fake_sensors.c, minus the "trigger
 * launch" half — replay launches itself by virtue of the recorded
 * timeline.
 */
static void autolaunch_task(void *, void *, void *)
{
	LOG_INF("replay autolaunch: watching for SM_LANDED");

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
