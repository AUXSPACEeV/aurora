#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Simulated rocket flight through the AURORA Kalman filter.

This script is a 1:1 Python mirror of the firmware's flight pipeline:

  * ``aurora/lib/filter/kalman.c``      → :class:`Filter` and the
    ``filter_init`` / ``filter_predict`` / ``filter_update`` /
    ``filter_detect_apogee`` free functions below.
  * ``aurora/lib/sensor/attitude.c``    → :class:`Attitude` and
    ``attitude_init`` / ``attitude_calibrate_sample`` /
    ``attitude_calibrate_finish`` / ``attitude_update`` /
    ``attitude_is_calibrated``.

Names, struct layout, return semantics (0 = OK, 1 = gated/apogee,
negative = errno-style error) and arithmetic order match the C as closely
as Python allows so that simulator behaviour tracks any change in the
firmware filters.  The Kconfig-derived constants (``CONFIG_FILTER_*``,
``CONFIG_IMU_UP_AXIS_*``) are reproduced as module-level integers using
the firmware defaults.

Plotting of the simulator output is delegated to ``plot_flight_data``;
this file is only concerned with the flight model and the filter mirrors.
"""

import argparse
import math
import errno
import numpy as np

import plot_flight_data as pfd


# ---------------------------------------------------------------------------
# Kconfig defaults (mirrors aurora/lib/filter/Kconfig and
# aurora/dts/bindings IMU axis selection)
# ---------------------------------------------------------------------------

FILTER_SCALE_DIVISOR = 1000.0
CONFIG_FILTER_Q_ALT_MILLISCALE = 100      # 0.1
CONFIG_FILTER_Q_VEL_MILLISCALE = 500      # 0.5
CONFIG_FILTER_R_MILLISCALE = 25000        # 25.0 — boosted from the firmware
                                          # default to track the noisier
                                          # synthetic baro signal.
CONFIG_FILTER_APOGEE_DEBOUNCE_SAMPLES = 3

# CONFIG_IMU_UP_AXIS_POS_Z: airframe +Z is the up axis.
CONFIG_IMU_UP_AXIS_INDEX = 2
CONFIG_IMU_UP_AXIS_SIGN = 1

# Mirrors of the #define constants in kalman.c.
FILTER_NIS_GATE = 25.0
FILTER_NIS_WARMUP = 30
FILTER_APOGEE_ACCEL_BAND = 20.0

# Mirrors ATTITUDE_NUM_AXES in attitude.h.
ATTITUDE_NUM_AXES = 3

# Standard gravity used everywhere downstream.
G0 = 9.80665


# ---------------------------------------------------------------------------
# Filter — mirrors struct filter and the filter_* API in kalman.c
# ---------------------------------------------------------------------------

class Filter:
    """Python mirror of ``struct filter`` from ``aurora/include/aurora/lib/filter.h``.

    Field names track the C struct exactly so the algorithm code below maps
    line-for-line to ``kalman.c``.
    """

    __slots__ = ("state", "covariance", "noise_p", "noise_m",
                 "peak_altitude", "last_accel_vert",
                 "consecutive_apogee", "apogee_latched",
                 "updates_since_init")

    def __init__(self):
        self.state = [0.0, 0.0]
        self.covariance = [[0.0, 0.0], [0.0, 0.0]]
        self.noise_p = [[0.0, 0.0], [0.0, 0.0]]
        self.noise_m = 0.0
        self.peak_altitude = 0.0
        self.last_accel_vert = 0.0
        self.consecutive_apogee = 0
        self.apogee_latched = 0
        self.updates_since_init = 0


def filter_init(f):
    """Mirror of ``filter_init`` in kalman.c."""
    if f is None:
        return -errno.EINVAL

    q_alt = CONFIG_FILTER_Q_ALT_MILLISCALE / FILTER_SCALE_DIVISOR
    q_vel = CONFIG_FILTER_Q_VEL_MILLISCALE / FILTER_SCALE_DIVISOR
    r_meas = CONFIG_FILTER_R_MILLISCALE / FILTER_SCALE_DIVISOR

    f.state[0] = 0.0
    f.state[1] = 0.0

    f.covariance[0][0] = 10.0
    f.covariance[0][1] = 0.0
    f.covariance[1][0] = 0.0
    f.covariance[1][1] = 10.0

    f.noise_p[0][0] = q_alt
    f.noise_p[0][1] = 0.0
    f.noise_p[1][0] = 0.0
    f.noise_p[1][1] = q_vel

    f.noise_m = r_meas

    f.peak_altitude = 0.0
    f.last_accel_vert = 0.0
    f.consecutive_apogee = 0
    f.apogee_latched = 0
    f.updates_since_init = 0

    return 0


def filter_predict(f, dt_ns, a_vert):
    """Mirror of ``filter_predict`` in kalman.c.

    ``dt_ns`` is the elapsed time in nanoseconds (matches the C
    ``int64_t dt`` parameter).
    """
    if f is None or dt_ns <= 0:
        return -errno.EINVAL

    dt_s = dt_ns / 1e9
    if dt_s > 1.0:
        return -errno.EINVAL

    altitude = f.state[0]
    velocity = f.state[1]

    f.state[0] = altitude + velocity * dt_s + 0.5 * a_vert * dt_s * dt_s
    f.state[1] = velocity + a_vert * dt_s
    f.last_accel_vert = a_vert

    Q00 = f.noise_p[0][0] * dt_s
    Q11 = f.noise_p[1][1] * dt_s

    P00 = f.covariance[0][0]
    P01 = f.covariance[0][1]
    P10 = f.covariance[1][0]
    P11 = f.covariance[1][1]

    f.covariance[0][0] = P00 + dt_s * (P10 + P01) + dt_s * dt_s * P11 + Q00
    f.covariance[0][1] = P01 + dt_s * P11
    f.covariance[1][0] = P10 + dt_s * P11
    f.covariance[1][1] = P11 + Q11

    return 0


def filter_update(f, z):
    """Mirror of ``filter_update`` in kalman.c.

    Returns 0 on apply, 1 if rejected by the NIS gate, ``-EDOM`` if the
    innovation covariance is singular, ``-EINVAL`` for a NULL filter.
    """
    if f is None:
        return -errno.EINVAL

    y = z - f.state[0]
    S = f.covariance[0][0] + f.noise_m

    if abs(S) < 1e-12:
        return -errno.EDOM

    f.updates_since_init += 1

    if (f.updates_since_init > FILTER_NIS_WARMUP
            and f.covariance[0][0] <= f.noise_m
            and y * y > FILTER_NIS_GATE * S):
        return 1

    K0 = f.covariance[0][0] / S
    K1 = f.covariance[1][0] / S

    f.state[0] += K0 * y
    f.state[1] += K1 * y

    P00 = f.covariance[0][0]
    P01 = f.covariance[0][1]
    P10 = f.covariance[1][0]
    P11 = f.covariance[1][1]

    f.covariance[0][0] = P00 - K0 * P00
    f.covariance[0][1] = P01 - K0 * P01
    f.covariance[1][0] = P10 - K1 * P00
    f.covariance[1][1] = P11 - K1 * P01

    return 0


def filter_detect_apogee(f):
    """Mirror of ``filter_detect_apogee`` in kalman.c."""
    if f is None:
        return -errno.EINVAL

    altitude = f.state[0]
    velocity = f.state[1]

    if altitude > f.peak_altitude:
        f.peak_altitude = altitude

    if f.apogee_latched:
        return 0

    velocity_ok = velocity <= 0.0
    descent_ok = altitude < f.peak_altitude
    inertial_ok = abs(f.last_accel_vert) < FILTER_APOGEE_ACCEL_BAND

    if velocity_ok and descent_ok and inertial_ok:
        f.consecutive_apogee += 1
    else:
        f.consecutive_apogee = 0

    if f.consecutive_apogee >= CONFIG_FILTER_APOGEE_DEBOUNCE_SAMPLES:
        f.apogee_latched = 1
        return 1

    return 0


def filter_votes(f):
    """Return the three-vote tuple as evaluated by the last
    ``filter_detect_apogee`` call.  Not part of the C API — exposed only
    for the simulator's plotting panel."""
    altitude = f.state[0]
    velocity = f.state[1]
    return (velocity <= 0.0,
            altitude < f.peak_altitude,
            abs(f.last_accel_vert) < FILTER_APOGEE_ACCEL_BAND)


# ---------------------------------------------------------------------------
# Attitude — mirrors struct attitude and the attitude_* API in attitude.c
# ---------------------------------------------------------------------------

class Attitude:
    """Python mirror of ``struct attitude`` from
    ``aurora/include/aurora/lib/attitude.h``."""

    __slots__ = ("g_b", "g_mag", "accel_bias", "gyro_bias",
                 "cal_samples", "cal_accel_sum", "cal_gyro_sum",
                 "calibrated")

    def __init__(self):
        self.g_b = [0.0] * ATTITUDE_NUM_AXES
        self.g_mag = 0.0
        self.accel_bias = [0.0] * ATTITUDE_NUM_AXES
        self.gyro_bias = [0.0] * ATTITUDE_NUM_AXES
        self.cal_samples = 0
        self.cal_accel_sum = [0.0] * ATTITUDE_NUM_AXES
        self.cal_gyro_sum = [0.0] * ATTITUDE_NUM_AXES
        self.calibrated = 0


def _vec3_norm(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def attitude_init(att):
    """Mirror of ``attitude_init`` in attitude.c."""
    if att is None:
        return -errno.EINVAL

    att.g_b = [0.0] * ATTITUDE_NUM_AXES
    att.accel_bias = [0.0] * ATTITUDE_NUM_AXES
    att.gyro_bias = [0.0] * ATTITUDE_NUM_AXES
    att.cal_accel_sum = [0.0] * ATTITUDE_NUM_AXES
    att.cal_gyro_sum = [0.0] * ATTITUDE_NUM_AXES
    att.cal_samples = 0
    att.calibrated = 0

    att.g_b[CONFIG_IMU_UP_AXIS_INDEX] = float(-CONFIG_IMU_UP_AXIS_SIGN)
    att.g_mag = G0

    return 0


def attitude_calibrate_sample(att, accel, gyro):
    """Mirror of ``attitude_calibrate_sample`` in attitude.c."""
    if att is None or accel is None or gyro is None:
        return -errno.EINVAL
    if att.calibrated:
        return -errno.EALREADY

    for i in range(ATTITUDE_NUM_AXES):
        att.cal_accel_sum[i] += accel[i]
        att.cal_gyro_sum[i] += gyro[i]
    att.cal_samples += 1
    return 0


def attitude_calibrate_finish(att):
    """Mirror of ``attitude_calibrate_finish`` in attitude.c."""
    if att is None:
        return -errno.EINVAL
    if att.cal_samples <= 0:
        return -errno.ENODATA

    n = float(att.cal_samples)
    accel_mean = [att.cal_accel_sum[i] / n for i in range(ATTITUDE_NUM_AXES)]
    for i in range(ATTITUDE_NUM_AXES):
        att.gyro_bias[i] = att.cal_gyro_sum[i] / n

    g_mag = _vec3_norm(accel_mean)
    if g_mag < 1e-6:
        g_mag = G0
    att.g_mag = g_mag

    att.g_b = [0.0] * ATTITUDE_NUM_AXES
    att.g_b[CONFIG_IMU_UP_AXIS_INDEX] = float(-CONFIG_IMU_UP_AXIS_SIGN)

    for i in range(ATTITUDE_NUM_AXES):
        att.accel_bias[i] = accel_mean[i] + g_mag * att.g_b[i]

    att.calibrated = 1
    return 0


def attitude_update(att, accel, gyro, dt_s):
    """Mirror of ``attitude_update`` in attitude.c.

    Differs from the C signature only in returning ``(rc, accel_vert)``
    rather than writing the result through an output pointer.
    """
    if att is None or accel is None or gyro is None or dt_s <= 0.0:
        return -errno.EINVAL, 0.0
    if not att.calibrated:
        return -errno.ENODATA, 0.0

    ax = accel[0] - att.accel_bias[0]
    ay = accel[1] - att.accel_bias[1]
    az = accel[2] - att.accel_bias[2]

    wx = (gyro[0] - att.gyro_bias[0]) * dt_s
    wy = (gyro[1] - att.gyro_bias[1]) * dt_s
    wz = (gyro[2] - att.gyro_bias[2]) * dt_s

    gx = att.g_b[0]
    gy = att.g_b[1]
    gz = att.g_b[2]

    nx = gx - (wy * gz - wz * gy)
    ny = gy - (wz * gx - wx * gz)
    nz = gz - (wx * gy - wy * gx)

    a_norm = math.sqrt(ax * ax + ay * ay + az * az)
    if a_norm > 1e-6:
        tau_s = 0.5
        sigma_r = 0.20
        r = (a_norm - att.g_mag) / att.g_mag
        w = math.exp(-0.5 * r * r / (sigma_r * sigma_r))
        gain = w * dt_s / tau_s
        inv = 1.0 / a_norm
        gmx = -ax * inv
        gmy = -ay * inv
        gmz = -az * inv
        nx += gain * (gmx - nx)
        ny += gain * (gmy - ny)
        nz += gain * (gmz - nz)

    n = math.sqrt(nx * nx + ny * ny + nz * nz)
    if n < 1e-9:
        return -errno.EDOM, 0.0

    att.g_b[0] = nx / n
    att.g_b[1] = ny / n
    att.g_b[2] = nz / n

    f_vert = -(ax * att.g_b[0] + ay * att.g_b[1] + az * att.g_b[2])
    accel_vert = f_vert - att.g_mag

    return 0, accel_vert


def attitude_is_calibrated(att):
    """Mirror of ``attitude_is_calibrated`` in attitude.c."""
    if att is None:
        return -errno.EINVAL
    return 1 if att.calibrated else 0


# ---------------------------------------------------------------------------
# Flight simulation
# ---------------------------------------------------------------------------

def simulate_flight(apogee_m=500.0, dt=0.02, seed=42, pre_launch_s=1.0):
    """Generate a synthetic rocket altitude profile, barometric pressure,
    and body-frame IMU stream.

    Phases:
      1. Pre-launch — stationary (used by attitude calibration).
      2. Boost      — constant thrust acceleration for ~3 s.
      3. Coast      — ballistic arc up to apogee.
      4. Descent    — drogue then main chute, with a slow pitch-over tumble
                      to exercise the gravity tracker.

    Returns ``(t, true_alt, true_press, noisy_press, accel_body, gyro_body,
    apogee_time)``.  The IMU stream is in body frame with +Z aligned to
    the airframe (matches ``CONFIG_IMU_UP_AXIS_POS_Z``).
    """
    rng = np.random.default_rng(seed)

    # Boost phase: solve for thrust accel that produces the target apogee
    # given a fixed burn time and gravity.
    burn_time = 3.0
    A_coeff = 0.5 * burn_time ** 2 / G0
    B_coeff = 0.5 * burn_time ** 2
    C_coeff = -apogee_m
    a_thrust = ((-B_coeff + math.sqrt(B_coeff ** 2 - 4 * A_coeff * C_coeff))
                / (2 * A_coeff))

    v_burnout = a_thrust * burn_time
    h_burnout = 0.5 * a_thrust * burn_time ** 2

    coast_time = v_burnout / G0
    computed_apogee = (h_burnout + v_burnout * coast_time
                       - 0.5 * G0 * coast_time ** 2)

    drogue_v = 30.0
    main_deploy_h = 150.0
    main_v = 6.0

    descent_time_drogue = (computed_apogee - main_deploy_h) / drogue_v
    descent_time_main = main_deploy_h / main_v
    flight_time = (burn_time + coast_time + descent_time_drogue
                   + descent_time_main + 2.0)
    total_time = pre_launch_s + flight_time

    t = np.arange(0, total_time, dt)
    altitude = np.zeros_like(t)
    accel_vert_world = np.zeros_like(t)

    for i, ti in enumerate(t):
        tf = ti - pre_launch_s
        if tf <= 0.0:
            altitude[i] = 0.0
            accel_vert_world[i] = 0.0
        elif tf <= burn_time:
            altitude[i] = 0.5 * a_thrust * tf ** 2
            accel_vert_world[i] = a_thrust - G0
        elif tf <= burn_time + coast_time:
            tc = tf - burn_time
            altitude[i] = h_burnout + v_burnout * tc - 0.5 * G0 * tc ** 2
            accel_vert_world[i] = -G0
        elif tf <= burn_time + coast_time + descent_time_drogue:
            td = tf - burn_time - coast_time
            altitude[i] = computed_apogee - drogue_v * td
            accel_vert_world[i] = 0.0
        elif tf <= (burn_time + coast_time + descent_time_drogue
                    + descent_time_main):
            tm = tf - burn_time - coast_time - descent_time_drogue
            altitude[i] = main_deploy_h - main_v * tm
            accel_vert_world[i] = 0.0
        else:
            altitude[i] = 0.0
            accel_vert_world[i] = 0.0

    altitude = np.maximum(altitude, 0.0)

    apogee_time = pre_launch_s + burn_time + coast_time
    pitch_rate = 0.35  # rad/s tumble around body-y after apogee
    pitch = np.where(t > apogee_time, pitch_rate * (t - apogee_time), 0.0)

    # Specific force in world frame: gravity-removed accel + g.
    specific_force_z_world = accel_vert_world + G0

    accel_body = np.zeros((len(t), 3))
    accel_body[:, 0] = np.sin(pitch) * specific_force_z_world
    accel_body[:, 2] = np.cos(pitch) * specific_force_z_world

    gyro_body = np.zeros((len(t), 3))
    gyro_body[t > apogee_time, 1] = pitch_rate

    accel_body += rng.normal(0.0, 0.4, size=accel_body.shape)
    gyro_body += rng.normal(0.0, 0.01, size=gyro_body.shape)

    true_pressure = pfd.altitude_to_pressure(altitude)
    pressure_noise = rng.normal(0.0, 50.0, size=len(t))
    noisy_pressure = true_pressure + pressure_noise

    return (t, altitude, true_pressure, noisy_pressure,
            accel_body, gyro_body, apogee_time)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def run_simulation(args):
    dt = 0.02  # 50 Hz
    dt_ns = int(dt * 1e9)
    pre_launch_s = 1.0
    (t, true_alt, true_press, noisy_press,
     accel_body, gyro_body, apogee_time) = simulate_flight(
        apogee_m=500.0, dt=dt, pre_launch_s=pre_launch_s)

    p_ref = noisy_press[0]
    baro_alt = pfd.pressure_to_altitude(noisy_press, p_ref)

    f = Filter()
    filter_init(f)
    att = Attitude()
    attitude_init(att)

    filtered_alt = np.zeros_like(t)
    filtered_vel = np.zeros_like(t)
    accel_vert_hist = np.zeros_like(t)
    g_b_hist = np.tile(np.array(att.g_b), (len(t), 1))
    vote_hist = np.zeros((len(t), 3), dtype=bool)
    gated_hist = np.zeros(len(t), dtype=bool)
    apogee_idx = None

    cal_samples = int(pre_launch_s / dt)
    for i in range(cal_samples):
        attitude_calibrate_sample(att, accel_body[i], gyro_body[i])
    attitude_calibrate_finish(att)

    settle_idx = cal_samples + int(1.0 / dt)
    gated_count = 0

    for i in range(len(t)):
        if i < cal_samples:
            a_vert = 0.0
        else:
            rc, a_vert = attitude_update(att, accel_body[i], gyro_body[i], dt)
            if rc != 0:
                a_vert = 0.0
        accel_vert_hist[i] = a_vert
        g_b_hist[i] = att.g_b

        filter_predict(f, dt_ns, a_vert)
        rc = filter_update(f, baro_alt[i])
        gated_hist[i] = (rc == 1)
        if rc == 1:
            gated_count += 1
        filtered_alt[i] = f.state[0]
        filtered_vel[i] = f.state[1]

        # Match the firmware: only run apogee detection once we are
        # well past the calibration / settle window.  In production the
        # gating is the BOOST / BURNOUT state entry; here it is a fixed
        # delay in samples.
        if i > settle_idx:
            detected = filter_detect_apogee(f)
            vote_hist[i] = filter_votes(f)
            if apogee_idx is None and detected == 1:
                apogee_idx = i

    true_vel = np.gradient(true_alt, t)
    computed_apogee_idx = int(np.argmax(filtered_alt))

    print(f"Ground ref pressure: {p_ref:.0f} Pa ({p_ref/100:.1f} hPa)")
    print(f"True apogee:         {true_alt.max():.1f} m "
          f"(t = {apogee_time:.2f} s)")
    if apogee_idx is not None:
        print(f"Filter apogee:       {filtered_alt[apogee_idx]:.1f} m "
              f"(t = {t[apogee_idx]:.2f} s)")
    else:
        print("Filter apogee:       not detected")
    print(f"Attitude g_mag:      {att.g_mag:.3f} m/s²")
    print(f"Gated baro updates:  {gated_count}")

    themes = ["light", "dark"] if args.theme == "both" else [args.theme]
    for theme in themes:
        suffix = f"_{theme}" if args.theme == "both" else ""
        out_path = f"flight_simulation{suffix}.png"
        pfd.plot_flight(
            t, noisy_press, baro_alt, filtered_alt, filtered_vel,
            g_b_hist, accel_vert_hist, vote_hist, apogee_idx,
            theme, out_path,
            true_alt=true_alt, true_press=true_press, true_vel=true_vel,
            computed_apogee_idx=computed_apogee_idx,
            accel_body=accel_body, gyro_body=gyro_body,
            gated_hist=gated_hist,
            title=(args.title if args.title is not None
                   else "AURORA Kalman Filter - Simulated 500 m Rocket Flight"))


def main():
    parser = argparse.ArgumentParser(
        description="Simulated rocket flight through the AURORA Kalman "
                    "filter (Python mirror of aurora/lib/filter and "
                    "aurora/lib/sensor/attitude.c).")
    parser.add_argument(
        "--theme", choices=["light", "dark", "both"], default="both",
        help="Colour theme matching the Furo Sphinx docs (default: both)")
    parser.add_argument(
        "--show", action="store_true",
        help="Open an interactive matplotlib window")
    parser.add_argument(
        "--title", type=str, default=None,
        help="Override the plot title")
    args = parser.parse_args()

    run_simulation(args)

    if args.show:
        import matplotlib.pyplot as plt
        plt.show()


if __name__ == "__main__":
    main()
