#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Simulated rocket flight through the AURORA Kalman filter.

Replicates the 2-state (altitude, velocity) constant-acceleration Kalman
filter from aurora/lib/filter/kalman.c together with the body-frame gravity
tracker from aurora/lib/sensor/attitude.c.  The simulation generates a
realistic pressure signal from the ISA barometric formula, synthesizes a
body-frame IMU (accel + gyro) with a slow pitch-over after apogee, and feeds
both streams through the Python mirrors of the flight pipeline.  Plots cover
pressure, altitude, vertical velocity, body-frame gravity direction, the
gravity-removed world-vertical acceleration, and the three-way apogee vote.

When invoked with ``--flight DIR``, the script instead loads real telemetry
from ``DIR/flights.influx`` and state transitions from ``DIR/state_audit``,
segments the log into individual flights using ARMED→BOOST transitions, and
produces one plot per flight with state transitions drawn as vertical lines.
"""

import argparse
import math
import os
import re
import numpy as np
import matplotlib as mpl
import matplotlib.pyplot as plt


# ---------------------------------------------------------------------------
# ISA barometric atmosphere (troposphere, h < 11 km)
# ---------------------------------------------------------------------------

P0 = 101325.0   # Sea-level standard pressure (Pa)
T0 = 288.15     # Sea-level standard temperature (K)
L  = 0.0065     # Temperature lapse rate (K/m)
g  = 9.80665    # Gravitational acceleration (m/s^2)
M  = 0.0289644  # Molar mass of dry air (kg/mol)
R  = 8.31447    # Universal gas constant (J/(mol·K))


def altitude_to_pressure(h):
    """ISA barometric formula: altitude (m) -> pressure (Pa)."""
    return P0 * (1 - L * h / T0) ** (g * M / (R * L))


def pressure_to_altitude(p, p_ref=P0):
    """Hypsometric formula: pressure (Pa) -> altitude (m) AGL.

    Uses a ground-level reference pressure p_ref so that the returned
    altitude is 0 m at launch.
    """
    return (T0 / L) * (1 - (p / p_ref) ** (R * L / (g * M)))


# ---------------------------------------------------------------------------
# Kalman filter - mirrors kalman.c exactly
# ---------------------------------------------------------------------------

# Mahalanobis (normalized innovation squared) gate: ~5-sigma.
FILTER_NIS_GATE = 25.0

# Updates to ignore before the NIS gate arms.
FILTER_NIS_WARMUP = 30

# Sanity band on world-frame vertical accel at the apogee decision point.
FILTER_APOGEE_ACCEL_BAND = 20.0


class KalmanFilter:
    """2-state Kalman filter identical to struct filter / kalman.c."""

    def __init__(self, q_alt=0.1, q_vel=0.5, r_meas=4.0,
                 apogee_debounce=3):
        # State vector [altitude, velocity]
        self.state = np.array([0.0, 0.0])

        # Covariance matrix P
        self.P = np.array([[10.0, 0.0],
                           [0.0, 10.0]])

        # Process noise (diagonal)
        self.Q = np.array([[q_alt, 0.0],
                           [0.0,  q_vel]])

        # Measurement noise variance R
        self.R = r_meas

        # Multi-criterion apogee detection state
        self.peak_altitude = 0.0
        self.last_accel_vert = 0.0
        self.consecutive_apogee = 0
        self.apogee_latched = False

        # Innovation-gate warm-up and bookkeeping for plots.
        self.updates_since_init = 0
        self.last_update_gated = False
        self.gated_updates = 0

        # Last vote values computed in detect_apogee() for plotting
        self.last_votes = (False, False, False)

        # Apogee detection parameters
        self.apogee_debounce = apogee_debounce

    def predict(self, dt_s: float, a_vert: float = 0.0):
        """filter_predict - constant-acceleration propagation with a_vert
        (gravity-removed world-frame vertical accel) as control input.
        Default 0.0 reduces to constant-velocity behavior."""
        self.state[0] += self.state[1] * dt_s + 0.5 * a_vert * dt_s**2
        self.state[1] += a_vert * dt_s
        self.last_accel_vert = a_vert

        P = self.P
        Q00 = self.Q[0, 0] * dt_s
        Q11 = self.Q[1, 1] * dt_s

        new_P = np.zeros((2, 2))
        new_P[0, 0] = P[0, 0] + dt_s * (P[1, 0] + P[0, 1]) + dt_s**2 * P[1, 1] + Q00
        new_P[0, 1] = P[0, 1] + dt_s * P[1, 1]
        new_P[1, 0] = P[1, 0] + dt_s * P[1, 1]
        new_P[1, 1] = P[1, 1] + Q11
        self.P = new_P

    def update(self, z: float) -> bool:
        """filter_update - measurement correction with innovation gate.

        Returns True if the measurement was applied, False if it was
        rejected by the NIS gate (glitch suppression).
        """
        y = z - self.state[0]
        S = self.P[0, 0] + self.R

        self.updates_since_init += 1

        if (self.updates_since_init > FILTER_NIS_WARMUP
                and self.P[0, 0] <= self.R
                and y * y > FILTER_NIS_GATE * S):
            self.last_update_gated = True
            self.gated_updates += 1
            return False

        self.last_update_gated = False

        K0 = self.P[0, 0] / S
        K1 = self.P[1, 0] / S

        self.state[0] += K0 * y
        self.state[1] += K1 * y

        P = self.P.copy()
        self.P[0, 0] = P[0, 0] - K0 * P[0, 0]
        self.P[0, 1] = P[0, 1] - K0 * P[0, 1]
        self.P[1, 0] = P[1, 0] - K1 * P[0, 0]
        self.P[1, 1] = P[1, 1] - K1 * P[0, 1]
        return True

    def detect_apogee(self) -> bool:
        """filter_detect_apogee - three-vote detector:
           (velocity <= 0) AND (altitude < peak) AND
           (|last_accel_vert| < FILTER_APOGEE_ACCEL_BAND),
           sustained for `apogee_debounce` consecutive calls.
           Velocity leads altitude at apogee so no hysteresis is used;
           debounce + the inertial sanity band reject noise.
           Latched once fired."""
        altitude = self.state[0]
        velocity = self.state[1]

        if altitude > self.peak_altitude:
            self.peak_altitude = altitude

        velocity_ok = velocity <= 0.0
        descent_ok = altitude < self.peak_altitude
        inertial_ok = abs(self.last_accel_vert) < FILTER_APOGEE_ACCEL_BAND
        self.last_votes = (velocity_ok, descent_ok, inertial_ok)

        if self.apogee_latched:
            return False

        if velocity_ok and descent_ok and inertial_ok:
            self.consecutive_apogee += 1
        else:
            self.consecutive_apogee = 0

        if self.consecutive_apogee >= self.apogee_debounce:
            self.apogee_latched = True
            return True
        return False


# ---------------------------------------------------------------------------
# Attitude tracker - mirrors lib/sensor/attitude.c (default +Z up mounting)
# ---------------------------------------------------------------------------

class Attitude:
    """Gyro-integrated body-frame gravity tracker.  Defaults mirror the
    CONFIG_IMU_UP_AXIS_POS_Z Kconfig choice: g_b seeded to [0, 0, -1]."""

    # Anchor time constant and relative magnitude 1-sigma band.  Fixed so
    # there are no knobs to tune: tau_s sets how quickly the accel-aided
    # correction cancels gyro drift; sigma_r shapes the Gaussian weight in
    # (|a| - g_mag)/g_mag that fades the correction during boost/deploy.
    TAU_S = 0.5
    SIGMA_R = 0.20

    def __init__(self):
        self.g_b = np.array([0.0, 0.0, -1.0])
        self.g_mag = 9.80665
        self.accel_bias = np.zeros(3)
        self.gyro_bias = np.zeros(3)
        self.cal_samples = 0
        self.cal_accel_sum = np.zeros(3)
        self.cal_gyro_sum = np.zeros(3)
        self.calibrated = False

    def calibrate_sample(self, accel, gyro):
        self.cal_accel_sum += accel
        self.cal_gyro_sum += gyro
        self.cal_samples += 1

    def calibrate_finish(self):
        n = float(self.cal_samples)
        accel_mean = self.cal_accel_sum / n
        self.gyro_bias = self.cal_gyro_sum / n
        g_mag = float(np.linalg.norm(accel_mean))
        if g_mag < 1e-6:
            g_mag = 9.80665
        self.g_mag = g_mag
        self.g_b = np.array([0.0, 0.0, -1.0])
        self.accel_bias = accel_mean + g_mag * self.g_b
        self.calibrated = True

    def update(self, accel, gyro, dt_s):
        """Returns gravity-removed world-vertical accel (positive = up)."""
        a = accel - self.accel_bias
        w = (gyro - self.gyro_bias) * dt_s
        g = self.g_b
        cross = np.array([w[1] * g[2] - w[2] * g[1],
                          w[2] * g[0] - w[0] * g[2],
                          w[0] * g[1] - w[1] * g[0]])
        new_g = g - cross

        # Complementary correction toward -a/|a|, Gaussian-weighted by how
        # close |a| is to g_mag.  Smooth weighting anchors during quasi-
        # static phases and fades out during boost/deploy shocks; gain is
        # dt/tau so behavior is independent of sample rate.
        a_norm = float(np.linalg.norm(a))
        if a_norm > 1e-6:
            r = (a_norm - self.g_mag) / self.g_mag
            weight = math.exp(-0.5 * (r / self.SIGMA_R) ** 2)
            gain = weight * dt_s / self.TAU_S
            g_meas = -a / a_norm
            new_g = new_g + gain * (g_meas - new_g)

        self.g_b = new_g / np.linalg.norm(new_g)
        f_vert = -float(np.dot(a, self.g_b))
        return f_vert - self.g_mag


# ---------------------------------------------------------------------------
# Flight simulation
# ---------------------------------------------------------------------------

def simulate_flight(apogee_m=500.0, dt=0.02, seed=42, pre_launch_s=1.0):
    """
    Generate a realistic rocket altitude profile and barometric pressure,
    along with a synthetic body-frame IMU stream.

    Phases:
      1. Pre-launch - stationary (used by the attitude tracker to calibrate)
      2. Boost      - constant thrust acceleration for ~3 s
      3. Coast      - ballistic arc (gravity only) up to apogee
      4. Descent    - drogue then main chute, with a slow pitch-over tumble

    Returns (time, true_altitude, true_pressure, noisy_pressure, accel_body,
             gyro_body, apogee_time) arrays.  The IMU streams are expressed
             in body frame with +Z aligned with the airframe (matches the
             CONFIG_IMU_UP_AXIS_POS_Z default).
    """
    rng = np.random.default_rng(seed)

    # --- Boost phase (constant acceleration) ---
    burn_time = 3.0  # seconds
    # Quadratic solve for thrust accel: 0.5*tb^2/g * a^2 + 0.5*tb^2 * a - apogee = 0
    A_coeff = 0.5 * burn_time**2 / g
    B_coeff = 0.5 * burn_time**2
    C_coeff = -apogee_m
    a_thrust = (-B_coeff + np.sqrt(B_coeff**2 - 4 * A_coeff * C_coeff)) / (2 * A_coeff)

    v_burnout = a_thrust * burn_time
    h_burnout = 0.5 * a_thrust * burn_time**2

    # --- Coast phase (gravity only until v=0) ---
    coast_time = v_burnout / g
    computed_apogee = h_burnout + v_burnout * coast_time - 0.5 * g * coast_time**2

    # --- Descent phase ---
    drogue_v = 30.0   # m/s under drogue
    main_deploy_h = 150.0
    main_v = 6.0      # m/s under main

    descent_time_drogue = (computed_apogee - main_deploy_h) / drogue_v
    descent_time_main = main_deploy_h / main_v
    flight_time = burn_time + coast_time + descent_time_drogue + descent_time_main + 2.0
    total_time = pre_launch_s + flight_time

    t = np.arange(0, total_time, dt)
    altitude = np.zeros_like(t)
    accel_vert_world = np.zeros_like(t)  # world-frame +up, gravity-removed

    for i, ti in enumerate(t):
        tf = ti - pre_launch_s  # flight-relative time
        if tf <= 0.0:
            altitude[i] = 0.0
            accel_vert_world[i] = 0.0
        elif tf <= burn_time:
            altitude[i] = 0.5 * a_thrust * tf**2
            accel_vert_world[i] = a_thrust - g
        elif tf <= burn_time + coast_time:
            tc = tf - burn_time
            altitude[i] = h_burnout + v_burnout * tc - 0.5 * g * tc**2
            accel_vert_world[i] = -g
        elif tf <= burn_time + coast_time + descent_time_drogue:
            td = tf - burn_time - coast_time
            altitude[i] = computed_apogee - drogue_v * td
            accel_vert_world[i] = 0.0  # terminal velocity under drogue
        elif tf <= burn_time + coast_time + descent_time_drogue + descent_time_main:
            tm = tf - burn_time - coast_time - descent_time_drogue
            altitude[i] = main_deploy_h - main_v * tm
            accel_vert_world[i] = 0.0  # terminal velocity under main
        else:
            altitude[i] = 0.0
            accel_vert_world[i] = 0.0

    altitude = np.maximum(altitude, 0.0)

    # --- Body attitude: nominally vertical, slow pitch-over after apogee ---
    apogee_time = pre_launch_s + burn_time + coast_time
    pitch_rate = 0.35  # rad/s tumble around body-y after apogee
    pitch = np.where(t > apogee_time, pitch_rate * (t - apogee_time), 0.0)

    # Specific force in world frame: a_world - gravity_world = [0, 0, a_vert + g]
    specific_force_z_world = accel_vert_world + g

    # Rotate world [0,0,f] into body via rotation of angle `pitch` around body-y.
    # body_x = sin(pitch) * f, body_y = 0, body_z = cos(pitch) * f
    accel_body = np.zeros((len(t), 3))
    accel_body[:, 0] = np.sin(pitch) * specific_force_z_world
    accel_body[:, 2] = np.cos(pitch) * specific_force_z_world

    gyro_body = np.zeros((len(t), 3))
    gyro_body[t > apogee_time, 1] = pitch_rate

    # IMU noise (body frame)
    accel_body += rng.normal(0.0, 0.4, size=accel_body.shape)
    gyro_body += rng.normal(0.0, 0.01, size=gyro_body.shape)

    # --- Pressure domain ---
    true_pressure = altitude_to_pressure(altitude)

    # MS5607 noise: ~50 Pa RMS at 4096 OSR (datasheet typical)
    pressure_noise = rng.normal(0.0, 50.0, size=len(t))
    noisy_pressure = true_pressure + pressure_noise

    return (t, altitude, true_pressure, noisy_pressure,
            accel_body, gyro_body, apogee_time)


# ---------------------------------------------------------------------------
# Real flight log parsing (InfluxDB line protocol + state_audit)
# ---------------------------------------------------------------------------

def parse_influx(path):
    """Parse AURORA telemetry in InfluxDB line protocol.

    Returns dict with keys ``accel``, ``gyro``, ``baro`` mapping to
    ``(t_ns, values)`` tuples where ``t_ns`` is a 1-D int array and
    ``values`` is an (N, K) float array (K=3 for accel/gyro, 2 for baro
    as ``[pres_kPa, temp_C]``).
    """
    rows = {"accel": ([], []), "gyro": ([], []), "baro": ([], [])}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            # "telemetry,type=T field=v,field=v <timestamp>"
            try:
                measurement_and_fields, ts = line.rsplit(" ", 1)
                meas_tags, fields = measurement_and_fields.split(" ", 1)
                t_ns = int(ts)
            except ValueError:
                continue
            tags = dict(kv.split("=", 1) for kv in meas_tags.split(",")[1:])
            ttype = tags.get("type")
            if ttype not in rows:
                continue
            fd = dict(kv.split("=", 1) for kv in fields.split(","))
            try:
                if ttype in ("accel", "gyro"):
                    vals = [float(fd["x"]), float(fd["y"]), float(fd["z"])]
                else:  # baro
                    vals = [float(fd["pres"]), float(fd["temp"])]
            except (KeyError, ValueError):
                continue
            rows[ttype][0].append(t_ns)
            rows[ttype][1].append(vals)

    out = {}
    for k, (ts, vs) in rows.items():
        out[k] = (np.array(ts, dtype=np.int64), np.array(vs, dtype=float))
    return out


def parse_state_audit(path):
    """Parse the state_audit file into a list of events.

    Each entry: ``(t_ns, kind, from_state, to_or_event)`` where ``kind`` is
    ``"transition"`` or ``"event"``. Tolerates the dash-prefixed first line.
    """
    rx = re.compile(r"^-*(\d+)\s+(\S+)\s+(\S+)\s+(.+)$")
    events = []
    with open(path) as f:
        for line in f:
            m = rx.match(line.strip())
            if not m:
                continue
            events.append((int(m.group(1)), m.group(2), m.group(3),
                           m.group(4).strip()))
    return events


def segment_flights(events):
    """Find all ARMED→BOOST transitions and the subsequent *→IDLE close-out.

    Returns list of ``(boost_ns, end_ns)`` tuples. ``end_ns`` may be ``None``
    if the flight is still running at end-of-log.
    """
    flights = []
    boosts = [i for i, e in enumerate(events)
              if e[1] == "transition" and e[2] == "ARMED" and e[3] == "BOOST"]
    for bi in boosts:
        boost_ns = events[bi][0]
        end_ns = None
        for ej in events[bi + 1:]:
            if ej[1] == "transition" and ej[3] == "IDLE":
                end_ns = ej[0]
                break
        flights.append((boost_ns, end_ns))
    return flights


def process_real_flight(streams, events, boost_ns, end_ns,
                        dt_s=0.02, pre_boost_s=10.0, cal_s=3.0,
                        post_end_s=2.0, post_main_s=5.0,
                        default_duration_s=120.0,
                        q_alt=0.1, q_vel=0.5, r_meas=4.0,
                        apogee_debounce=3):
    """Run the real flight window through Attitude + KalmanFilter.

    Aligns t=0 to ``boost_ns - pre_boost_s*1000000000`` so that the first
    ``cal_s`` seconds are stationary and usable for calibration, and the
    BOOST transition lands at ``t = pre_boost_s``.

    If a ``*→MAIN`` transition is present within ``[boost_ns, end_ns]``,
    the analysis window is truncated to ``MAIN + post_main_s`` seconds to
    discard the long post-parachute tail (where the state machine often
    idles out instead of progressing to REDUNDANT).

    Returns a dict with keys: t, pressure_pa, baro_alt, filtered_alt,
    filtered_vel, g_b_hist, accel_vert_hist, vote_hist, apogee_idx,
    computed_apogee_idx, state_transitions, g_mag, p_ref.
    """
    if cal_s > pre_boost_s:
        print(f"  warning: cal_s={cal_s:.1f}s > pre_boost_s={pre_boost_s:.1f}s, "
              f"clamping cal_s to {pre_boost_s:.1f}s")
        cal_s = pre_boost_s

    window_start_ns = boost_ns - int(pre_boost_s * 1000000000)
    raw_end_ns = (end_ns if end_ns is not None
                  else boost_ns + int(default_duration_s * 1000000000))

    main_ns = None
    for ev in events:
        if ev[0] < boost_ns or ev[0] > raw_end_ns:
            continue
        if ev[1] == "transition" and ev[3] == "MAIN":
            main_ns = ev[0]
            break

    if main_ns is not None:
        tail_ns = main_ns + int(post_main_s * 1000000000)
    else:
        tail_ns = raw_end_ns
    window_end_ns = tail_ns + int(post_end_s * 1000000000)

    duration_s = (window_end_ns - window_start_ns) / 1000000000.0
    t = np.arange(0.0, duration_s, dt_s)

    def resample(t_ns_raw, vals_raw):
        mask = (t_ns_raw >= window_start_ns) & (t_ns_raw <= window_end_ns)
        ts = (t_ns_raw[mask] - window_start_ns) / 1000000000.0
        vs = vals_raw[mask]
        out = np.zeros((len(t), vs.shape[1]))
        for k in range(vs.shape[1]):
            out[:, k] = np.interp(t, ts, vs[:, k])
        return out

    accel = resample(*streams["accel"])
    gyro = resample(*streams["gyro"])
    baro = resample(*streams["baro"])  # [pres_kPa, temp_C]

    pressure_pa = baro[:, 0] * 1000.0  # kPa → Pa

    # Ground reference: mean pressure over the calibration window.
    cal_samples = int(cal_s / dt_s)
    p_ref = float(np.mean(pressure_pa[:cal_samples]))
    baro_alt = pressure_to_altitude(pressure_pa, p_ref)

    kf = KalmanFilter(q_alt=q_alt, q_vel=q_vel, r_meas=r_meas,
                      apogee_debounce=apogee_debounce)
    att = Attitude()

    for i in range(cal_samples):
        att.calibrate_sample(accel[i], gyro[i])
    att.calibrate_finish()

    filtered_alt = np.zeros_like(t)
    filtered_vel = np.zeros_like(t)
    accel_vert_hist = np.zeros_like(t)
    g_b_hist = np.tile(att.g_b, (len(t), 1))
    vote_hist = np.zeros((len(t), 3), dtype=bool)
    gated_hist = np.zeros(len(t), dtype=bool)
    apogee_idx = None

    # Skip apogee detection until the KF settles past calibration.
    settle_idx = cal_samples + int(1.0 / dt_s)

    for i in range(len(t)):
        if i < cal_samples:
            a_vert = 0.0
        else:
            a_vert = att.update(accel[i], gyro[i], dt_s)
        accel_vert_hist[i] = a_vert
        g_b_hist[i] = att.g_b

        kf.predict(dt_s, a_vert)
        kf.update(baro_alt[i])
        gated_hist[i] = kf.last_update_gated
        filtered_alt[i] = kf.state[0]
        filtered_vel[i] = kf.state[1]

        # Match the state machine: only run apogee detection once past
        # the settle window.  In production this gating is the BOOST /
        # BURNOUT state entry; here we approximate with a fixed delay.
        if i > settle_idx:
            detected = kf.detect_apogee()
            vote_hist[i] = kf.last_votes
            if apogee_idx is None and detected:
                apogee_idx = i

    state_transitions = []
    for ev in events:
        if ev[1] != "transition":
            continue
        if window_start_ns <= ev[0] <= window_end_ns:
            ts = (ev[0] - window_start_ns) / 1000000000.0
            state_transitions.append((ts, ev[2], ev[3]))

    computed_apogee_idx = (int(np.argmax(filtered_alt))
                         if len(filtered_alt) > 0 else None)

    return {
        "t": t,
        "pressure_pa": pressure_pa,
        "baro_alt": baro_alt,
        "filtered_alt": filtered_alt,
        "filtered_vel": filtered_vel,
        "g_b_hist": g_b_hist,
        "accel_vert_hist": accel_vert_hist,
        "accel_body": accel,
        "gyro_body": gyro,
        "vote_hist": vote_hist,
        "gated_hist": gated_hist,
        "gated_updates": kf.gated_updates,
        "apogee_idx": apogee_idx,
        "computed_apogee_idx": computed_apogee_idx,
        "state_transitions": state_transitions,
        "g_mag": att.g_mag,
        "p_ref": p_ref,
    }


# ---------------------------------------------------------------------------
# Furo-matched colour themes (from Sphinx conf.py)
# ---------------------------------------------------------------------------

THEMES = {
    "light": {
        "bg":           "#ffffff",
        "fg":           "#333333",
        "grid":         "#dce0e5",
        "brand":        "#1a5fb4",
        "noisy":        "#bbbbbb",
        "pressure":     "#9141ac",
        "true_alt":     "#1a5fb4",
        "filtered":     "#e5a50a",
        "velocity":     "#26a269",
        "velocity_ref": "#8fd19e",
        "apogee":       "#c01c28",
        "zero_line":    "#333333",
        "g_x":          "#c01c28",
        "g_y":          "#26a269",
        "g_z":          "#1a5fb4",
        "accel_vert":   "#813d9c",
        "vote_vel":     "#26a269",
        "vote_desc":    "#1a5fb4",
        "vote_acc":     "#e5a50a",
        "legend_bg":    "#f8f9fa",
        "legend_edge":  "#dce0e5",
    },
    "dark": {
        "bg":           "#1e2028",
        "fg":           "#dcdee3",
        "grid":         "#3a3d46",
        "brand":        "#6ea8fe",
        "noisy":        "#505460",
        "pressure":     "#c77dff",
        "true_alt":     "#6ea8fe",
        "filtered":     "#ffd43b",
        "velocity":     "#51cf66",
        "velocity_ref": "#2f7a3d",
        "apogee":       "#ff6b6b",
        "zero_line":    "#8890a0",
        "g_x":          "#ff6b6b",
        "g_y":          "#51cf66",
        "g_z":          "#6ea8fe",
        "accel_vert":   "#c77dff",
        "vote_vel":     "#51cf66",
        "vote_desc":    "#6ea8fe",
        "vote_acc":     "#ffd43b",
        "legend_bg":    "#252830",
        "legend_edge":  "#3a3d46",
    },
}


def apply_theme(name: str) -> dict:
    """Configure matplotlib rcParams to match a Furo theme and return the
    colour palette."""
    t = THEMES[name]
    mpl.rcParams.update({
        "figure.facecolor":   t["bg"],
        "axes.facecolor":     t["bg"],
        "axes.edgecolor":     t["grid"],
        "axes.labelcolor":    t["fg"],
        "text.color":         t["fg"],
        "xtick.color":        t["fg"],
        "ytick.color":        t["fg"],
        "legend.facecolor":   t["legend_bg"],
        "legend.edgecolor":   t["legend_edge"],
        "legend.labelcolor":  t["fg"],
        "savefig.facecolor":  t["bg"],
    })
    return t


# ---------------------------------------------------------------------------
# Plotting
# ---------------------------------------------------------------------------

def plot_flight(t, noisy_press, baro_alt, filtered_alt, filtered_vel,
                g_b_hist, accel_vert_hist, vote_hist, apogee_idx,
                theme_name, out_path, true_alt=None, true_press=None,
                true_vel=None, state_transitions=None, title=None,
                computed_apogee_idx=None, accel_body=None,
                gyro_body=None, gated_hist=None):
    """Generate the 6-panel flight plot using the given theme.

    Optional overlays:
      * ``true_alt`` / ``true_press`` / ``true_vel`` — ground-truth reference
        curves (only available for the simulation path).
      * ``state_transitions`` — list of ``(t_s, from_state, to_state)`` tuples
        drawn as labelled vertical lines on every panel.
      * ``computed_apogee_idx`` — index of the filtered-altitude peak, drawn
        as a thin light reference line for comparison with ``apogee_idx``.
      * ``gated_hist`` — boolean array marking baro samples that were
        rejected by the innovation gate; drawn as scatter on the pressure
        panel.
    """
    c = apply_theme(theme_name)

    fig, axes = plt.subplots(
        7, 1, figsize=(12, 18), sharex=True,
        gridspec_kw={"height_ratios": [1.5, 2.5, 1.5, 1.5, 1.5, 1.5, 1.2]},
    )
    ax_press, ax_alt, ax_vel, ax_att, ax_gyro, ax_acc, ax_vote = axes

    state_palette = {
        "IDLE":      "#9a9996",
        "ARMED":     "#e5a50a",
        "BOOST":     "#c01c28",
        "BURNOUT":   "#ed333b",
        "APOGEE":    "#9141ac",
        "MAIN":      "#1a5fb4",
        "REDUNDANT": "#26a269",
        "LANDED":    "#613583",
        "ERROR":     "#a51d2d",
    }

    def mark_apogee(ax, label=None):
        if apogee_idx is None:
            return
        ax.axvline(t[apogee_idx], color=c["apogee"],
                   linestyle="--", linewidth=1.5, label=label)

    def mark_computed_apogee(ax, label=None):
        if computed_apogee_idx is None:
            return
        ax.axvline(t[computed_apogee_idx], color=c["apogee"],
                   linestyle="-", linewidth=0.8, alpha=0.35, label=label)

    def mark_states(ax, with_labels=False, with_dt_apogee=False):
        if not state_transitions:
            return None
        ymin, ymax = ax.get_ylim()
        for ts, frm, to in state_transitions:
            col = state_palette.get(to, c["zero_line"])
            if with_dt_apogee and to == "APOGEE":
                dt_apogee = ts - t[computed_apogee_idx]
                label = (f"Apogee detected (flight) (t={ts:.2f} s, "
                         f"Δ={dt_apogee:+.2f} s)")
                print(label)
                ax.axvline(ts, color=col, linestyle="-", linewidth=1.0, alpha=0.7, label=label)
            else:
                ax.axvline(ts, color=col, linestyle="-", linewidth=1.0, alpha=0.7)
            if with_labels:
                ax.text(ts, ymax, f" {to}", color=col, fontsize=8,
                        va="top", ha="left", rotation=90,
                        bbox=dict(boxstyle="round,pad=0.15",
                                facecolor=c["legend_bg"],
                                edgecolor=col, linewidth=0.5, alpha=0.85))

    # 1) Pressure
    ax_press.plot(t, noisy_press / 100, color=c["noisy"], linewidth=0.5,
                  label="Baro pressure")
    if true_press is not None:
        ax_press.plot(t, true_press / 100, color=c["pressure"], linewidth=1.5,
                      label="True pressure")
    if gated_hist is not None and np.any(gated_hist):
        idx = np.flatnonzero(gated_hist)
        ax_press.scatter(t[idx], noisy_press[idx] / 100,
                         color=c["apogee"], marker="x", s=28, linewidths=1.2,
                         label=f"NIS-gated ({len(idx)})", zorder=5)
    mark_computed_apogee(ax_press)
    mark_apogee(ax_press)
    ax_press.set_ylabel("Pressure (hPa)")
    if title is not None:
        ax_press.set_title(title, color=c["brand"], fontweight="bold")
    ax_press.legend(loc="upper right")
    ax_press.grid(True, color=c["grid"], alpha=0.5)
    ax_press.invert_yaxis()
    mark_states(ax_press, with_labels=True)

    # 2) Altitude
    ax_alt.plot(t, baro_alt, color=c["noisy"], linewidth=0.5,
                label="Baro altitude (hypsometric)")
    if true_alt is not None:
        ax_alt.plot(t, true_alt, color=c["true_alt"], linewidth=2,
                    label="True altitude")
    ax_alt.plot(t, filtered_alt, color=c["filtered"], linewidth=2,
                label="Kalman filter output")
    if computed_apogee_idx is not None:
        mark_computed_apogee(ax_alt,
                           f"Computed apogee (t={t[computed_apogee_idx]:.2f} s)")
    if apogee_idx is not None:
        if computed_apogee_idx is not None:
            dt_apogee = t[apogee_idx] - t[computed_apogee_idx]
            apogee_label = (f"Apogee detected (simulator) (t={t[apogee_idx]:.2f} s, "
                            f"Δ={dt_apogee:+.2f} s)")
        else:
            apogee_label = f"Apogee detected (simulator) (t={t[apogee_idx]:.2f} s)"
        mark_apogee(ax_alt, apogee_label)
        ax_alt.plot(t[apogee_idx], filtered_alt[apogee_idx], "o",
                    color=c["apogee"], markersize=8)
    ax_alt.set_ylabel("Altitude (m)")
    ax_alt.grid(True, color=c["grid"], alpha=0.5)
    mark_states(ax_alt, with_dt_apogee=True)
    ax_alt.legend(loc="upper right")

    # 3) Vertical velocity (filtered + optional truth reference)
    if true_vel is not None:
        ax_vel.plot(t, true_vel, color=c["velocity_ref"], linewidth=1.0,
                    linestyle=":", label="True velocity (d/dt)")
    ax_vel.plot(t, filtered_vel, color=c["velocity"], linewidth=1.5,
                label="Filtered velocity")
    ax_vel.axhline(0, color=c["zero_line"], linewidth=0.5)
    mark_computed_apogee(ax_vel)
    mark_apogee(ax_vel)
    ax_vel.set_ylabel("Velocity (m/s)")
    ax_vel.legend(loc="upper right")
    ax_vel.grid(True, color=c["grid"], alpha=0.5)
    mark_states(ax_vel)

    # 4) Attitude: body-frame gravity direction g_b
    ax_att.plot(t, g_b_hist[:, 0], color=c["g_x"], linewidth=1.5,
                label="g_b[x]")
    ax_att.plot(t, g_b_hist[:, 1], color=c["g_y"], linewidth=1.5,
                label="g_b[y]")
    ax_att.plot(t, g_b_hist[:, 2], color=c["g_z"], linewidth=1.5,
                label="g_b[z]")
    ax_att.axhline(0, color=c["zero_line"], linewidth=0.5)
    mark_computed_apogee(ax_att)
    mark_apogee(ax_att)
    ax_att.set_ylabel("Gravity dir (body)")
    ax_att.set_ylim(-1.15, 1.15)
    ax_att.legend(loc="upper right", ncol=3)
    ax_att.grid(True, color=c["grid"], alpha=0.5)
    mark_states(ax_att)

    # 5) Body-frame gyro rates
    if gyro_body is not None:
        ax_gyro.plot(t, gyro_body[:, 0], color=c["g_x"], linewidth=1.0,
                     label="ω_body[x]")
        ax_gyro.plot(t, gyro_body[:, 1], color=c["g_y"], linewidth=1.0,
                     label="ω_body[y]")
        ax_gyro.plot(t, gyro_body[:, 2], color=c["g_z"], linewidth=1.0,
                     label="ω_body[z]")
    ax_gyro.axhline(0, color=c["zero_line"], linewidth=0.5)
    mark_computed_apogee(ax_gyro)
    mark_apogee(ax_gyro)
    ax_gyro.set_ylabel("ω (rad/s)")
    ax_gyro.legend(loc="upper right", ncol=3)
    ax_gyro.grid(True, color=c["grid"], alpha=0.5)
    mark_states(ax_gyro)

    # 6) Gravity-removed world-vertical accel (attitude.update output)
    if accel_body is not None:
        ax_acc.plot(t, accel_body[:, 0], color=c["g_x"], linewidth=0.8,
                    alpha=0.35, label="a_body[x]")
        ax_acc.plot(t, accel_body[:, 1], color=c["g_y"], linewidth=0.8,
                    alpha=0.35, label="a_body[y]")
        ax_acc.plot(t, accel_body[:, 2], color=c["g_z"], linewidth=0.8,
                    alpha=0.35, label="a_body[z]")
    ax_acc.plot(t, accel_vert_hist, color=c["accel_vert"], linewidth=1.2,
                label="a_vert (world, g-removed)")
    ax_acc.axhline(0, color=c["zero_line"], linewidth=0.5)
    mark_computed_apogee(ax_acc)
    mark_apogee(ax_acc)
    ax_acc.set_ylabel("a_vert (m/s²)")
    ax_acc.legend(loc="upper right")
    ax_acc.grid(True, color=c["grid"], alpha=0.5)
    mark_states(ax_acc)

    # 7) Apogee votes - step plots at stacked y-levels
    vote_labels = [
        "velocity <= -k·σ_vel",
        "altitude < peak",
        "|a_vert| < 20 m/s²",
    ]
    vote_colors = [c["vote_vel"], c["vote_desc"], c["vote_acc"]]
    n_votes = len(vote_labels)
    for row, (label, color) in enumerate(zip(vote_labels, vote_colors)):
        base = (n_votes - 1) - row
        ax_vote.fill_between(t, base, base + vote_hist[:, row].astype(float) * 0.8,
                             step="pre", color=color, alpha=0.55, linewidth=0,
                             label=label)
    mark_computed_apogee(ax_vote)
    mark_apogee(ax_vote)
    ax_vote.set_xlabel("Time (s)")
    ax_vote.set_ylabel("Apogee votes")
    ax_vote.set_yticks([0.4 + i for i in range(n_votes)])
    ax_vote.set_yticklabels(list(reversed(vote_labels)))
    ax_vote.set_ylim(-0.1, n_votes)
    ax_vote.legend(loc="upper right", ncol=3, fontsize=8)
    ax_vote.grid(True, color=c["grid"], alpha=0.5, axis="x")
    mark_states(ax_vote)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"[{theme_name}] Plot saved to {out_path}")
    return fig


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run_simulation(args):
    dt = 0.02  # 50 Hz sample rate
    pre_launch_s = 1.0
    (t, true_alt, true_press, noisy_press,
     accel_body, gyro_body, apogee_time) = simulate_flight(
        apogee_m=500.0, dt=dt, pre_launch_s=pre_launch_s)

    p_ref = noisy_press[0]
    baro_alt = pressure_to_altitude(noisy_press, p_ref)

    kf = KalmanFilter(q_alt=0.1, q_vel=0.5, r_meas=25.0)
    att = Attitude()

    filtered_alt = np.zeros_like(t)
    filtered_vel = np.zeros_like(t)
    accel_vert_hist = np.zeros_like(t)
    g_b_hist = np.tile(att.g_b, (len(t), 1))
    vote_hist = np.zeros((len(t), 3), dtype=bool)
    gated_hist = np.zeros(len(t), dtype=bool)
    apogee_idx = None

    cal_samples = int(pre_launch_s / dt)
    for i in range(cal_samples):
        att.calibrate_sample(accel_body[i], gyro_body[i])
    att.calibrate_finish()

    settle_idx = cal_samples + int(1.0 / dt)

    for i in range(len(t)):
        if i < cal_samples:
            a_vert = 0.0
        else:
            a_vert = att.update(accel_body[i], gyro_body[i], dt)
        accel_vert_hist[i] = a_vert
        g_b_hist[i] = att.g_b

        kf.predict(dt, a_vert)
        kf.update(baro_alt[i])
        gated_hist[i] = kf.last_update_gated
        filtered_alt[i] = kf.state[0]
        filtered_vel[i] = kf.state[1]

        # Match the state machine: only run apogee detection once past
        # the settle window.  In production this gating is the BOOST /
        # BURNOUT state entry; here we approximate with a fixed delay.
        if i > settle_idx:
            detected = kf.detect_apogee()
            vote_hist[i] = kf.last_votes
            if apogee_idx is None and detected:
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
    print(f"Gated baro updates:  {kf.gated_updates}")

    themes = ["light", "dark"] if args.theme == "both" else [args.theme]
    for theme in themes:
        suffix = f"_{theme}" if args.theme == "both" else ""
        out_path = f"flight_simulation{suffix}.png"
        plot_flight(t, noisy_press, baro_alt, filtered_alt, filtered_vel,
                    g_b_hist, accel_vert_hist, vote_hist, apogee_idx,
                    theme, out_path,
                    true_alt=true_alt, true_press=true_press, true_vel=true_vel,
                    computed_apogee_idx=computed_apogee_idx,
                    accel_body=accel_body, gyro_body=gyro_body,
                    gated_hist=gated_hist,
                    title="AURORA Kalman Filter - Simulated 500 m Rocket Flight")


def run_real_flight(args):
    flight_dir = args.flight
    influx_path = os.path.join(flight_dir, "flights.influx")
    audit_path = os.path.join(flight_dir, "state_audit")

    for p in (influx_path, audit_path):
        if not os.path.isfile(p):
            raise SystemExit(f"error: missing {p}")

    print(f"Loading telemetry from {influx_path} ...")
    streams = parse_influx(influx_path)
    print(f"  accel: {len(streams['accel'][0])} samples")
    print(f"  gyro:  {len(streams['gyro'][0])} samples")
    print(f"  baro:  {len(streams['baro'][0])} samples")

    events = parse_state_audit(audit_path)
    flights = segment_flights(events)
    if not flights:
        raise SystemExit("error: no ARMED->BOOST transitions found")
    print(f"Detected {len(flights)} flight(s):")
    for n, (b, e) in enumerate(flights, start=1):
        end_str = f"{e} ms" if e is not None else "end-of-log"
        print(f"  flight {n}: BOOST @ {b} ms → close @ {end_str}")

    themes = ["light", "dark"] if args.theme == "both" else [args.theme]

    for n, (boost_ns, end_ns) in enumerate(flights, start=1):
        print(f"\n--- Processing flight {n} ---")
        result = process_real_flight(streams, events, boost_ns, end_ns,
                                     dt_s=args.dt, pre_boost_s=args.pre_boost,
                                     cal_s=args.cal,
                                     post_main_s=args.post_main,
                                     q_alt=args.q_alt, q_vel=args.q_vel,
                                     r_meas=args.r_meas,
                                     apogee_debounce=args.debounce)

        apogee_idx = result["apogee_idx"]
        computed_idx = result["computed_apogee_idx"]
        print(f"  p_ref:         {result['p_ref']:.0f} Pa "
              f"({result['p_ref']/100:.1f} hPa)")
        print(f"  g_mag:         {result['g_mag']:.3f} m/s²")
        print(f"  peak baro alt: {result['baro_alt'].max():.1f} m")
        if computed_idx is not None:
            print(f"  computed apogee: {result['filtered_alt'][computed_idx]:.1f} m "
                  f"(t = {result['t'][computed_idx]:.2f} s)")
        if apogee_idx is not None:
            delta = (result['t'][apogee_idx] - result['t'][computed_idx]
                     if computed_idx is not None else None)
            delta_str = f", Δ={delta:+.2f} s" if delta is not None else ""
            print(f"  filter apogee: {result['filtered_alt'][apogee_idx]:.1f} m "
                  f"(t = {result['t'][apogee_idx]:.2f} s{delta_str})")
        else:
            print("  filter apogee: not detected")
        print(f"  transitions:   {len(result['state_transitions'])}")
        print(f"  gated baro:    {result['gated_updates']}")

        title = f"AURORA Flight {n} - {os.path.basename(flight_dir.rstrip('/'))}"
        for theme in themes:
            suffix = f"_{theme}" if args.theme == "both" else ""
            out_path = f"flight{n}{suffix}.png"
            plot_flight(result["t"], result["pressure_pa"],
                        result["baro_alt"], result["filtered_alt"],
                        result["filtered_vel"], result["g_b_hist"],
                        result["accel_vert_hist"], result["vote_hist"],
                        apogee_idx, theme, out_path,
                        state_transitions=result["state_transitions"],
                        computed_apogee_idx=result["computed_apogee_idx"],
                        accel_body=result["accel_body"],
                        gyro_body=result["gyro_body"],
                        gated_hist=result["gated_hist"],
                        title=title)


def main():
    parser = argparse.ArgumentParser(
        description="Simulated or recorded rocket flight through the "
                    "AURORA Kalman filter.")
    parser.add_argument(
        "--theme", choices=["light", "dark", "both"], default="both",
        help="Colour theme matching the Furo Sphinx docs (default: both)")
    parser.add_argument(
        "--show", action="store_true",
        help="Open an interactive matplotlib window")
    parser.add_argument(
        "--flight", metavar="DIR",
        help="Load real flight data from DIR (expects flights.influx + "
             "state_audit). Produces one plot per flight segmented on "
             "ARMED→BOOST transitions.")
    parser.add_argument(
        "--dt", type=float, default=0.02,
        help="Resampling period in seconds for real flight data (default 0.02)")
    parser.add_argument(
        "--pre-boost", type=float, default=10.0, dest="pre_boost",
        help="Seconds of pre-boost data to include per flight (default 10)")
    parser.add_argument(
        "--cal", type=float, default=3.0,
        help="Seconds of calibration window before BOOST (default 3)")
    parser.add_argument(
        "--post-main", type=float, default=5.0, dest="post_main",
        help="Seconds of data to keep after the MAIN transition "
             "(default 5) — trims the long post-parachute tail")
    parser.add_argument("--q-alt", type=float, default=0.1, dest="q_alt",
                        help="KF process noise on altitude (default 0.1)")
    parser.add_argument("--q-vel", type=float, default=0.5, dest="q_vel",
                        help="KF process noise on velocity (default 0.5, "
                             "matches Kconfig)")
    parser.add_argument("--r-meas", type=float, default=4.0, dest="r_meas",
                        help="KF measurement noise variance "
                             "(default 4.0, matches Kconfig)")
    parser.add_argument("--debounce", type=int, default=3,
                        help="Apogee debounce ticks (default 3, "
                             "matches Kconfig)")
    args = parser.parse_args()

    if args.flight:
        run_real_flight(args)
    else:
        run_simulation(args)

    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
