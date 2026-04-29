/**
 * @file fake_sensors.c
 * @brief Synthetic IMU and baro data source for sim-based testing.
 *
 * When CONFIG_AURORA_FAKE_SENSORS is enabled, replaces the real polling
 * threads in main.c with a canned flight profile generator. Publishes on
 * the same zbus channels (imu_data_chan, baro_data_chan) at the same
 * cadence and priorities, so downstream code (attitude, Kalman filter,
 * state machine, data logger) runs unchanged. Works on native_sim and on
 * real hardware targets; layer via the `sim` snippet (-S sim).
 *
 * The profile is kicked off by the shell command `sim launch` and can
 * be returned to pad-stationary with `sim reset`. Altitude and pressure
 * follow ISA troposphere; accelerometer reports proper acceleration
 * (includes +g on Z when stationary or under parachute, ~0 in free-fall).
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aurora/lib/state/state.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log_ctrl.h>

#include <aurora/lib/imu.h>
#include <aurora/lib/baro.h>

#ifndef M_PI
#define M_PI ((double)3.14159265358979323846)
#endif

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
/* Roll rate (rotation about the rocket's long axis) injected into the
 * gyro after launch.  Must be non-zero so the autotest can verify roll
 * is being integrated.  Held at zero pre-launch so attitude calibration
 * sees a stationary IMU and captures a zero gyro bias.
 */
#define ROLL_RATE_DPS            30.0
/* Parachute descent rate applied after apogee. */
#define DROGUE_RATE_MS           12.0
#define MAIN_RATE_MS             4.0

/* ISA troposphere, matching the constants in lib/sensor/baro.c */
#define SEA_LEVEL_PRESSURE_KPA   101.325
#define ISA_T0                   288.15
#define ISA_L                    0.0065
#define ISA_GMR_OVER_L           5.25588

/* -------- Calibration signal (written by main.c, read by autolaunch) -------- */

static struct k_spinlock cal_lock;
static bool calibration_done;

/**
 * @brief Notify the automatic launch sequence that attitude calibration is complete.
 */
void fake_sensors_on_calibrated(void)
{
	k_spinlock_key_t key = k_spin_lock(&cal_lock);
	calibration_done = true;
	k_spin_unlock(&cal_lock, key);
}

/* -------- Launch trigger state (shared with shell command) -------- */

static struct k_spinlock profile_lock;
/** Uptime (ms) at which `sim launch` fired. 0 means not yet launched. */
static uint64_t launch_uptime_ns;

static uint64_t profile_launch_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&profile_lock);
	uint64_t v = launch_uptime_ns;
	k_spin_unlock(&profile_lock, key);
	return v;
}

static void profile_launch_set(uint64_t v)
{
	k_spinlock_key_t key = k_spin_lock(&profile_lock);
	launch_uptime_ns = v;
	k_spin_unlock(&profile_lock, key);
}

/* -------- Profile math -------- */

/**
 * @brief Compute altitude (AGL) and proper Z-axis acceleration at flight time.
 *
 * @param t_s Seconds since launch; negative means stationary on the pad.
 */
static void profile_sample(double t_s, double *altitude_m, double *accel_vert_ms2)
{
	static double t_to_main = 0.0;

	if (t_s < 0.0) {
		*altitude_m = 0.0;
		*accel_vert_ms2 = GRAVITY_MS2;
		return;
	}

	if (t_s < BOOST_DURATION_S) {
		*altitude_m = 0.5 * BOOST_COORD_ACCEL_MS2 * t_s * t_s;
		*accel_vert_ms2 = BOOST_COORD_ACCEL_MS2 + GRAVITY_MS2;
		return;
	}

	const double v_burnout = BOOST_COORD_ACCEL_MS2 * BOOST_DURATION_S;
	const double h_burnout = 0.5 * BOOST_COORD_ACCEL_MS2 *
				 BOOST_DURATION_S * BOOST_DURATION_S;
	const double t_coast = t_s - BOOST_DURATION_S;
	const double v = v_burnout - GRAVITY_MS2 * t_coast;
	const enum sm_state state = sm_get_state();

	if (state < SM_APOGEE) {
		if (v < 0.0) {
			LOG_WRN_ONCE("Simulator APOGEE; State machine %s", sm_state_str(state));
		}
		*altitude_m = h_burnout + v_burnout * t_coast -
			      0.5 * GRAVITY_MS2 * t_coast * t_coast;
		*accel_vert_ms2 = 0.0; /* ballistic coast: accelerometer reads 0 */
		return;
	} else if (state == SM_MAIN && t_to_main == 0.0) {
		t_to_main = t_s;
	}

	/* After apogee: approximate instant parachute, constant descent rate. */
	const double t_to_apogee = v_burnout / GRAVITY_MS2;
	const double h_apogee = h_burnout + v_burnout * t_to_apogee -
				0.5 * GRAVITY_MS2 * t_to_apogee * t_to_apogee;
	const double t_after_apogee = t_coast - t_to_apogee;
	const double t_main = t_to_main == 0.0 ? 0.0 : t_s - t_to_main;
	const double t_apogee_to_main = t_after_apogee - t_main;

	double h = h_apogee - DROGUE_RATE_MS * t_apogee_to_main -
		   MAIN_RATE_MS * t_main;
	if (h < 0.0) {
		h = 0.0;
	}
	*altitude_m = h;
	*accel_vert_ms2 = GRAVITY_MS2; /* terminal descent or landed */
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
	uint64_t launch = profile_launch_get();
	if (launch == 0) {
		return -1.0;
	}
	return (double)(k_ticks_to_ns_floor64(k_uptime_ticks()) - launch) / 1000000000.0;
}

/* -------- Synthetic IMU thread -------- */

static void fake_imu_task(void *, void *, void *)
{
	const int hz = CONFIG_IMU_FREQUENCY;
	const uint64_t period_ns = 1000000000 / hz;

	LOG_INF("Fake IMU running at %d Hz", hz);
	imu_active = true;

	while (1) {
		uint64_t start = k_ticks_to_ns_floor64(k_uptime_ticks());
		double altitude, accel_vert;
		profile_sample(flight_time_seconds(), &altitude, &accel_vert);

		/* Inject a constant roll rate about the configured up axis once
		 * the flight is in progress (t_s >= 0).  Pre-launch the gyro
		 * stays at zero so the attitude calibration window captures a
		 * zero bias.
		 */
		const double t_s = flight_time_seconds();
		double gyro_axes[3] = {0.0, 0.0, 0.0};
		if (t_s >= 0.0) {
			gyro_axes[CONFIG_IMU_UP_AXIS_INDEX] =
				ROLL_RATE_DPS * (M_PI / 180.0) *
				(double)CONFIG_IMU_UP_AXIS_SIGN;
		}

		struct imu_data msg = {0};
		set_sensor_value_double(&msg.accel[0], 0.0);
		set_sensor_value_double(&msg.accel[1], accel_vert);
		set_sensor_value_double(&msg.accel[2], 0.0);
		set_sensor_value_double(&msg.gyro[0], gyro_axes[0]);
		set_sensor_value_double(&msg.gyro[1], gyro_axes[1]);
		set_sensor_value_double(&msg.gyro[2], gyro_axes[2]);

		(void)zbus_chan_pub(&imu_data_chan, &msg, K_NO_WAIT);
		uint64_t delta = k_ticks_to_ns_floor64(k_uptime_ticks()) - start;

		k_sleep(K_NSEC(period_ns - delta));
	}
}

K_THREAD_DEFINE(imu_polling, 2048, fake_imu_task, NULL, NULL, NULL,
		5, 0, 0);

/* -------- Synthetic baro thread -------- */

static void fake_baro_task(void *, void *, void *)
{
	const int hz = CONFIG_BARO_FREQUENCY;
	const uint64_t period_ns = 1000000000 / hz;

	LOG_INF("Fake baro running at %d Hz", hz);
	baro_active = true;

	while (1) {
		uint64_t start = k_ticks_to_ns_floor64(k_uptime_ticks());
		double altitude, accel_vert;
		profile_sample(flight_time_seconds(), &altitude, &accel_vert);

		struct baro_data msg = {0};
		set_sensor_value_double(&msg.temperature, 20.0);
		set_sensor_value_double(&msg.pressure,
				       altitude_to_pressure_kpa(altitude));

		(void)zbus_chan_pub(&baro_data_chan, &msg, K_NO_WAIT);

		uint64_t delta = k_ticks_to_ns_floor64(k_uptime_ticks()) - start;

		k_sleep(K_NSEC(period_ns - delta));
	}
}

K_THREAD_DEFINE(baro_polling, 2048, fake_baro_task, NULL, NULL, NULL,
		5, 0, 0);

/* -------- Shell interface -------- */

static int cmd_sim_launch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	profile_launch_set(k_ticks_to_ns_floor64(k_uptime_ticks()));
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
	double altitude, accel_vert;
	profile_sample(t, &altitude, &accel_vert);

	if (t < 0.0) {
		shell_print(sh, "sim: pad-stationary");
	} else {
		shell_print(sh, "sim: t=%.2fs  h=%.2fm  a_vert=%.2fm/s^2",
			    t, altitude, accel_vert);
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

#if defined(CONFIG_AURORA_SIM_AUTOTEST)
/**
 * @brief run an automatic launch sequence and exit when it lands or hits an error.
 */
static void autolaunch_task(void *, void *, void *)
{
	LOG_INF("autolaunch: waiting for attitude calibration...");
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

	LOG_INF("autolaunch: launching flight profile");
	profile_launch_set(k_ticks_to_ns_floor64(k_uptime_ticks()));

	int64_t deadline = k_uptime_get() + CONFIG_AURORA_SIM_AUTOLAUNCH_TIMEOUT_MS;

	while (1) {
		enum sm_state s = sm_get_state();

		if (s == SM_LANDED) {
			/* Sanity-check that yaw, pitch and roll were tracked
			 * end-to-end.  With the fake profile the rocket stays
			 * vertical (yaw ~ 0, pitch ~ 0) and spins about the up
			 * axis at ROLL_RATE_DPS, so roll must have moved away
			 * from zero by the time we land.
			 */
			struct sm_inputs sm_in;
			sm_get_inputs(&sm_in);
			LOG_INF("autolaunch: orientation yaw=%.2f pitch=%.2f roll=%.2f",
				sm_in.orientation[0], sm_in.orientation[1],
				sm_in.orientation[2]);
			__ASSERT(fabs(sm_in.orientation[0]) < 5.0,
				 "autolaunch: yaw drift %.2f deg exceeds 5 deg",
				 sm_in.orientation[0]);
			__ASSERT(fabs(sm_in.orientation[1]) < 5.0,
				 "autolaunch: pitch drift %.2f deg exceeds 5 deg",
				 sm_in.orientation[1]);
			__ASSERT(fabs(sm_in.orientation[2]) > 1.0,
				 "autolaunch: roll did not integrate (%.2f deg)",
				 sm_in.orientation[2]);

			LOG_INF("autolaunch: LANDED - simulation complete");
			log_flush();
			exit(0);
		}
		if (s == SM_ERROR) {
			/* __ASSERT_NO_MSG in error handler likely fires first */
			LOG_ERR("autolaunch: ERROR state - simulation failed");
			log_flush();
			exit(1);
		}
		if (k_uptime_get() >= deadline) {
			LOG_ERR("autolaunch: ERROR - timeout after %d ms without landing",
				CONFIG_AURORA_SIM_AUTOLAUNCH_TIMEOUT_MS);
			log_flush();
			exit(1);
		}
		k_msleep(100);
	}
}

K_THREAD_DEFINE(autolaunch, 1024, autolaunch_task, NULL, NULL, NULL, 5, 0, 0);

#endif /* CONFIG_AURORA_SIM_AUTOTEST */
