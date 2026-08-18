/**
 * @file sim.h
 * @brief Launch clock shared by the simulated IMU and baro sample sources.
 *
 * The simulated sources live in imu.c and baro.c, next to the hardware paths
 * they stand in for.  Both need to agree on when the flight started, so that
 * one clock -- and the `sim` shell command set that drives it -- lives here.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_SIM_H_
#define AURORA_LIB_SIM_H_

#include <stdint.h>
#include <zephyr/drivers/sensor.h>

/**
 * @brief Instant the simulated flight started, in uptime ns.
 *
 * @return 0 while pad-stationary.  Replay cursors also use the value as a
 *         generation counter: it changes on every launch/reset, which is
 *         their cue to rewind.
 */
uint64_t sim_launch_origin(void);

/** @brief Seconds since launch; negative while pad-stationary. */
double sim_flight_time_s(void);

/** @brief Start the simulated flight from now. */
void sim_launch(void);

/** @brief Return to pad-stationary. */
void sim_reset(void);

/** @brief Split a double into a sensor_value. */
static inline void sim_set_sensor_value(struct sensor_value *sv, double v)
{
	sv->val1 = (int32_t)v;
	sv->val2 = (int32_t)((v - (double)sv->val1) * 1000000.0);
}

#if defined(CONFIG_AURORA_FAKE_SENSORS_SYNTH)
/** Roll rate injected about the up axis after launch, degrees/s. */
#define SIM_ROLL_RATE_DPS 30.0

/**
 * @brief Altitude AGL and proper vertical acceleration at flight time @p t_s.
 *
 * Boost, ballistic coast and parachute descent, following the state machine's
 * own view of the flight for the events it cannot derive from time alone.
 *
 * @param t_s            Seconds since launch; negative means pad-stationary.
 * @param altitude_m     Altitude above the pad, metres.
 * @param accel_vert_ms2 Proper vertical acceleration (includes +g when
 *                       stationary or under parachute, ~0 in free-fall).
 */
void sim_profile_sample(double t_s, double *altitude_m, double *accel_vert_ms2);

/** @brief ISA troposphere pressure at altitude @p h_m, in kPa. */
double sim_altitude_to_pressure_kpa(double h_m);
#endif /* CONFIG_AURORA_FAKE_SENSORS_SYNTH */

#endif /* AURORA_LIB_SIM_H_ */
