/**
 * @file sim.c
 * @brief Launch clock and `sim` shell commands for the simulated sensors.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/sim.h>

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <aurora/lib/state/state.h>

static struct k_spinlock launch_lock;
static uint64_t launch_uptime_ns;

/* sim_launch_origin - see sim.h */
uint64_t sim_launch_origin(void)
{
	k_spinlock_key_t key = k_spin_lock(&launch_lock);
	uint64_t v = launch_uptime_ns;

	k_spin_unlock(&launch_lock, key);
	return v;
}

static void sim_origin_set(uint64_t v)
{
	k_spinlock_key_t key = k_spin_lock(&launch_lock);

	launch_uptime_ns = v;
	k_spin_unlock(&launch_lock, key);
}

/* sim_launch - see sim.h */
void sim_launch(void)
{
	sim_origin_set(k_ticks_to_ns_floor64(k_uptime_ticks()));
}

/* sim_reset - see sim.h */
void sim_reset(void)
{
	sim_origin_set(0);
}

/* sim_flight_time_s - see sim.h */
double sim_flight_time_s(void)
{
	uint64_t origin = sim_launch_origin();

	if (origin == 0) {
		return -1.0;
	}

	return (double)(k_ticks_to_ns_floor64(k_uptime_ticks()) - origin) / 1e9;
}

#if defined(CONFIG_AURORA_FAKE_SENSORS_SYNTH)

LOG_MODULE_REGISTER(sim, CONFIG_AURORA_SENSORS_LOG_LEVEL);

#define GRAVITY_MS2           9.81
/* Coordinate (not proper) acceleration produced by the motor during boost. */
#define BOOST_COORD_ACCEL_MS2 30.0
#define BOOST_DURATION_S      1.5
#define DROGUE_RATE_MS        12.0
#define MAIN_RATE_MS          4.0

/* ISA troposphere, matching the constants in baro.c. */
#define SEA_LEVEL_PRESSURE_KPA 101.325
#define ISA_T0                 288.15
#define ISA_L                  0.0065
#define ISA_GMR_OVER_L         5.25588

/* sim_profile_sample - see sim.h */
void sim_profile_sample(double t_s, double *altitude_m, double *accel_vert_ms2)
{
	static double t_to_main;

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
			LOG_WRN_ONCE("Simulator APOGEE; state machine %s",
				     sm_state_str(state));
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
	const double t_main = (t_to_main == 0.0) ? 0.0 : t_s - t_to_main;
	const double t_apogee_to_main = t_after_apogee - t_main;

	double h = h_apogee - DROGUE_RATE_MS * t_apogee_to_main -
		   MAIN_RATE_MS * t_main;

	*altitude_m = (h < 0.0) ? 0.0 : h;
	*accel_vert_ms2 = GRAVITY_MS2; /* terminal descent or landed */
}

/* sim_altitude_to_pressure_kpa - see sim.h */
double sim_altitude_to_pressure_kpa(double h_m)
{
	const double ratio = 1.0 - ISA_L * h_m / ISA_T0;

	if (ratio <= 0.0) {
		return 0.01;
	}

	return SEA_LEVEL_PRESSURE_KPA * pow(ratio, ISA_GMR_OVER_L);
}

#endif /* CONFIG_AURORA_FAKE_SENSORS_SYNTH */

static int cmd_sim_launch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sim_launch();
	shell_print(sh, "sim: flight launched");
	return 0;
}

static int cmd_sim_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sim_reset();
	shell_print(sh, "sim: reset to pad-stationary");
	return 0;
}

static int cmd_sim_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	double t = sim_flight_time_s();

	if (t < 0.0) {
		shell_print(sh, "sim: pad-stationary");
	} else {
		shell_print(sh, "sim: t=%.2fs", t);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sim_subcmds,
	SHELL_CMD(launch, NULL, "Start the simulated flight", cmd_sim_launch),
	SHELL_CMD(reset, NULL, "Reset back to pad-stationary", cmd_sim_reset),
	SHELL_CMD(status, NULL, "Print the simulated flight time", cmd_sim_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sim, &sim_subcmds, "Simulated flight control", NULL);
