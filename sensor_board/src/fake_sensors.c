/**
 * @file fake_sensors.c
 * @brief Synthetic IMU and baro data source for native_sim-based testing.
 *
 * When CONFIG_AURORA_FAKE_SENSORS is enabled, replaces the real polling
 * threads in main.c with a canned flight profile generator. Publishes on
 * the same zbus channels (imu_data_chan, baro_data_chan) at the same
 * cadence and priorities, so downstream code (attitude, Kalman filter,
 * state machine, data logger) runs unchanged.
 *
 * The profile is kicked off by the shell command `sim launch` and can
 * be returned to pad-stationary with `sim reset`. Altitude and pressure
 * follow ISA troposphere; accelerometer reports proper acceleration
 * (includes +g on Z when stationary or under parachute, ~0 in free-fall).
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/imu.h>
#include <aurora/lib/baro.h>

LOG_MODULE_REGISTER(fake_sensors, CONFIG_SENSOR_BOARD_LOG_LEVEL);

/* baro_active / imu_active are owned by main.c; the state machine waits on
 * them before entering its loop.
 */
extern bool baro_active;
extern bool imu_active;

/* -------- Flight profile constants -------- */

#define GRAVITY_MS2              9.81
/* Coordinate (not proper) acceleration produced by the motor during boost. */
#define BOOST_COORD_ACCEL_MS2    30.0
#define BOOST_DURATION_S         1.5
/* Parachute descent rate applied after apogee. */
#define PARACHUTE_RATE_MS         3.0

/* ISA troposphere, matching the constants in lib/sensor/baro.c */
#define SEA_LEVEL_PRESSURE_KPA   101.325
#define ISA_T0                   288.15
#define ISA_L                    0.0065
#define ISA_GMR_OVER_L           5.25588

/* -------- Launch trigger state (shared with shell command) -------- */

static struct k_spinlock profile_lock;
/** Uptime (ms) at which `sim launch` fired. 0 means not yet launched. */
static int64_t launch_uptime_ms;

static int64_t profile_launch_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&profile_lock);
	int64_t v = launch_uptime_ms;
	k_spin_unlock(&profile_lock, key);
	return v;
}

static void profile_launch_set(int64_t v)
{
	k_spinlock_key_t key = k_spin_lock(&profile_lock);
	launch_uptime_ms = v;
	k_spin_unlock(&profile_lock, key);
}

/* -------- Profile math -------- */

/**
 * @brief Compute altitude (AGL) and proper Z-axis acceleration at flight time.
 *
 * @param t_s Seconds since launch; negative means stationary on the pad.
 */
static void profile_sample(double t_s, double *altitude_m, double *accel_z_ms2)
{
	if (t_s < 0.0) {
		*altitude_m = 0.0;
		*accel_z_ms2 = GRAVITY_MS2;
		return;
	}

	if (t_s < BOOST_DURATION_S) {
		*altitude_m = 0.5 * BOOST_COORD_ACCEL_MS2 * t_s * t_s;
		*accel_z_ms2 = BOOST_COORD_ACCEL_MS2 + GRAVITY_MS2;
		return;
	}

	const double v_burnout = BOOST_COORD_ACCEL_MS2 * BOOST_DURATION_S;
	const double h_burnout = 0.5 * BOOST_COORD_ACCEL_MS2 *
				 BOOST_DURATION_S * BOOST_DURATION_S;
	const double t_coast = t_s - BOOST_DURATION_S;

	const double v = v_burnout - GRAVITY_MS2 * t_coast;
	if (v > 0.0) {
		*altitude_m = h_burnout + v_burnout * t_coast -
			      0.5 * GRAVITY_MS2 * t_coast * t_coast;
		*accel_z_ms2 = 0.0; /* ballistic coast: accelerometer reads 0 */
		return;
	}

	/* After apogee: approximate instant parachute, constant descent rate. */
	const double t_to_apogee = v_burnout / GRAVITY_MS2;
	const double h_apogee = h_burnout + v_burnout * t_to_apogee -
				0.5 * GRAVITY_MS2 * t_to_apogee * t_to_apogee;
	const double t_after_apogee = t_coast - t_to_apogee;

	double h = h_apogee - PARACHUTE_RATE_MS * t_after_apogee;
	if (h < 0.0) {
		h = 0.0;
	}
	*altitude_m = h;
	*accel_z_ms2 = GRAVITY_MS2; /* terminal descent or landed */
}

static double altitude_to_pressure_kpa(double h_m)
{
	const double ratio = 1.0 - ISA_L * h_m / ISA_T0;
	if (ratio <= 0.0) {
		return 0.01;
	}
	return SEA_LEVEL_PRESSURE_KPA * pow(ratio, ISA_GMR_OVER_L);
}

static void set_sensor_value_double(struct sensor_value *sv, double v)
{
	sv->val1 = (int32_t)v;
	sv->val2 = (int32_t)((v - (double)sv->val1) * 1000000.0);
}

static double flight_time_seconds(void)
{
	int64_t launch = profile_launch_get();
	if (launch == 0) {
		return -1.0;
	}
	return (double)(k_uptime_get() - launch) / 1000.0;
}

/* -------- Synthetic IMU thread -------- */

static void fake_imu_task(void *, void *, void *)
{
	const int hz = CONFIG_IMU_FREQUENCY_VALUE;
	const int period_ms = 1000 / hz;

	LOG_INF("Fake IMU running at %d Hz", hz);
	imu_active = true;

	while (1) {
		double altitude, accel_z;
		profile_sample(flight_time_seconds(), &altitude, &accel_z);

		struct imu_data msg = {0};
		set_sensor_value_double(&msg.accel[0], 0.0);
		set_sensor_value_double(&msg.accel[1], accel_z);
		set_sensor_value_double(&msg.accel[2], 0.0);
		set_sensor_value_double(&msg.gyro[0], 0.0);
		set_sensor_value_double(&msg.gyro[1], 0.0);
		set_sensor_value_double(&msg.gyro[2], 0.0);

		(void)zbus_chan_pub(&imu_data_chan, &msg, K_NO_WAIT);

		k_sleep(K_MSEC(period_ms));
	}
}

K_THREAD_DEFINE(imu_polling, 2048, fake_imu_task, NULL, NULL, NULL,
		5, 0, 0);

/* -------- Synthetic baro thread -------- */

static void fake_baro_task(void *, void *, void *)
{
	const int hz = CONFIG_BARO_FREQUENCY_VALUE;
	const int period_ms = 1000 / hz;

	LOG_INF("Fake baro running at %d Hz", hz);
	baro_active = true;

	while (1) {
		double altitude, accel_z;
		profile_sample(flight_time_seconds(), &altitude, &accel_z);

		struct baro_data msg = {0};
		set_sensor_value_double(&msg.temperature, 20.0);
		set_sensor_value_double(&msg.pressure,
				       altitude_to_pressure_kpa(altitude));

		(void)zbus_chan_pub(&baro_data_chan, &msg, K_NO_WAIT);

		k_sleep(K_MSEC(period_ms));
	}
}

K_THREAD_DEFINE(baro_polling, 2048, fake_baro_task, NULL, NULL, NULL,
		5, 0, 0);

/* -------- Shell interface -------- */

static int cmd_sim_launch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	profile_launch_set(k_uptime_get());
	shell_print(sh, "sim: flight profile launched");
	return 0;
}

static int cmd_sim_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	profile_launch_set(0);
	shell_print(sh, "sim: flight profile reset (pad-stationary)");
	return 0;
}

static int cmd_sim_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	double t = flight_time_seconds();
	double altitude, accel_z;
	profile_sample(t, &altitude, &accel_z);

	if (t < 0.0) {
		shell_print(sh, "sim: pad-stationary");
	} else {
		shell_print(sh, "sim: t=%.2fs  h=%.2fm  a_z=%.2fm/s^2",
			    t, altitude, accel_z);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sim_subcmds,
	SHELL_CMD(launch, NULL,
		  "Start the synthetic flight profile",
		  cmd_sim_launch),
	SHELL_CMD(reset, NULL,
		  "Reset the profile back to pad-stationary",
		  cmd_sim_reset),
	SHELL_CMD(status, NULL,
		  "Print current synthetic-flight state",
		  cmd_sim_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sim, &sim_subcmds,
		   "Synthetic flight-profile simulator", NULL);
