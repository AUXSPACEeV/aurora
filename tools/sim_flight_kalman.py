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
plots the raw streams (accel, gyro, baro, on-board sm_kinematics and
sm_pose) for each flight with state transitions drawn as vertical lines.
The NIS gate and the three-vote apogee detector are replayed from the
logged streams alone — gating is approximated under a steady-state
assumption since the on-board covariance is not telemetered.
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

FIELD_SPECS = {
    "accel":         ("x", "y", "z"),
    "gyro":          ("x", "y", "z"),
    "baro":          ("pres", "temp"),
    "sm_kinematics": ("accel", "accel_vert", "velocity"),
    "sm_pose":       ("orientation", "altitude"),
}


def parse_influx(path):
    """Parse AURORA telemetry in InfluxDB line protocol.

    Returns dict mapping each known type in :data:`FIELD_SPECS` to a
    ``(t_ns, values)`` tuple where ``t_ns`` is a 1-D int64 array and
    ``values`` is an (N, K) float array with columns in the order given
    by :data:`FIELD_SPECS`.
    """
    rows = {k: ([], []) for k in FIELD_SPECS}
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
            spec = FIELD_SPECS.get(ttype)
            if spec is None:
                continue
            try:
                fd = dict(kv.split("=", 1) for kv in fields.split(","))
                vals = [float(fd[k]) for k in spec]
            except (KeyError, ValueError):
                continue
            rows[ttype][0].append(t_ns)
            rows[ttype][1].append(vals)

    out = {}
    for k, (ts, vs) in rows.items():
        out[k] = (np.array(ts, dtype=np.int64),
                  np.array(vs, dtype=float).reshape(-1, len(FIELD_SPECS[k])))
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
            if ej[1] == "transition" and (ej[3] == "IDLE" or ej[3] == "LANDED"):
                end_ns = ej[0]
                break
        flights.append((boost_ns, end_ns))
    return flights


def slice_real_flight(streams, events, boost_ns, end_ns,
                      pre_boost_s=10.0, post_end_s=2.0,
                      default_duration_s=120.0):
    """Slice the raw telemetry streams to a single flight window.

    The window starts ``pre_boost_s`` seconds before the BOOST transition
    and ends ``post_end_s`` seconds after the flight close-out (or after
    ``default_duration_s`` if the audit log never closes the flight). All
    timestamps are returned in seconds, with t=0 at the window start so
    the BOOST transition lands at t = ``pre_boost_s``.

    Returns ``(sliced_streams, transitions)`` where ``sliced_streams``
    maps each stream name to ``(t_s, values)`` with t_s in seconds, and
    ``transitions`` is a list of ``(t_s, from_state, to_state)``.
    """
    window_start_ns = boost_ns - int(pre_boost_s * 1000000000)
    raw_end_ns = (end_ns if end_ns is not None
                  else boost_ns + int(default_duration_s * 1000000000))
    window_end_ns = raw_end_ns + int(post_end_s * 1000000000)

    sliced = {}
    for name, (t_ns_raw, vals_raw) in streams.items():
        mask = (t_ns_raw >= window_start_ns) & (t_ns_raw <= window_end_ns)
        t_s = (t_ns_raw[mask] - window_start_ns) / 1000000000.0
        sliced[name] = (t_s, vals_raw[mask])

    transitions = []
    for ev in events:
        if ev[1] != "transition":
            continue
        if window_start_ns <= ev[0] <= window_end_ns:
            ts = (ev[0] - window_start_ns) / 1000000000.0
            transitions.append((ts, ev[2], ev[3]))

    return sliced, transitions


# ---------------------------------------------------------------------------
# Replay helpers: derive apogee votes and NIS-gating from logged streams
# ---------------------------------------------------------------------------

def compute_log_votes(sliced):
    """Replay the three apogee votes from sm_kinematics + sm_pose alone.

    Returns ``(t_s, votes)`` where ``votes`` is an (N, 3) bool array with
    columns matching :func:`KalmanFilter.detect_apogee` — velocity ≤ 0,
    altitude < running peak, |accel_vert| < FILTER_APOGEE_ACCEL_BAND.
    Returns ``(None, None)`` if either stream is empty.
    """
    t_kin, kin = sliced["sm_kinematics"]
    t_pose, pose = sliced["sm_pose"]
    if t_kin.size == 0 or t_pose.size == 0:
        return None, None
    altitude = np.interp(t_kin, t_pose, pose[:, 1])
    accel_vert = kin[:, 1]
    velocity = kin[:, 2]
    peak = np.maximum.accumulate(altitude)
    votes = np.column_stack([
        velocity <= 0.0,
        altitude < peak,
        np.abs(accel_vert) < FILTER_APOGEE_ACCEL_BAND,
    ])
    return t_kin, votes


def compute_log_gated(sliced, r_meas, warmup=FILTER_NIS_WARMUP):
    """Approximate the firmware NIS gate from logged baro + sm_pose only.

    The on-board P[0,0] is not telemetered, so we assume the steady-state
    case (P[0,0] has converged below R, S ≈ R) and flag baro samples whose
    residual against the logged Kalman altitude exceeds the firmware gate
    threshold.  Returns ``(t_s, gated, baro_alt)`` with ``gated`` a bool
    array over the baro timeline; ``(None, None, None)`` if either stream
    is empty.
    """
    t_baro, baro = sliced["baro"]
    t_pose, pose = sliced["sm_pose"]
    if t_baro.size == 0 or t_pose.size == 0:
        return None, None, None
    # Logged baro pressure is in kPa (matches the *10 → hPa scaling in
    # plot_raw_flight); the ISA helpers expect Pa.
    p_pa = baro[:, 0] * 1000.0
    baro_alt = pressure_to_altitude(p_pa, p_pa[0])
    kalman_alt = np.interp(t_baro, t_pose, pose[:, 1])
    y = baro_alt - kalman_alt
    gated = (y * y) > FILTER_NIS_GATE * r_meas
    if warmup > 0:
        gated[:warmup] = False
    return t_baro, gated, baro_alt


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


STATE_PALETTE = {
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


def plot_raw_flight(sliced, transitions, theme_name, out_path, title=None,
                    r_meas=6.0, disable_votes=False):
    """Plot every raw stream found in ``sliced`` against time, with state
    transitions drawn as labelled vertical lines on every panel.

    ``sliced`` is the dict returned by :func:`slice_real_flight`. Panels
    are created only for stream types that have at least one sample in
    the window.

    ``r_meas`` is the assumed measurement variance used to approximate the
    NIS gate from the logged baro and Kalman-altitude streams; it should
    match ``CONFIG_FILTER_R_MILLISCALE / 1000`` for the flight build.
    """
    c = apply_theme(theme_name)

    t_gated, gated_mask, baro_alt = compute_log_gated(sliced, r_meas=r_meas)
    t_votes, votes = compute_log_votes(sliced)

    panels = []
    if sliced["baro"][0].size:
        panels.append("baro")
    if sliced["sm_pose"][0].size:
        panels.append("sm_pose")
    if sliced["sm_kinematics"][0].size:
        panels.append("velocity")
        panels.append("sm_accel")
    if sliced["accel"][0].size:
        panels.append("body_accel")
    if sliced["gyro"][0].size:
        panels.append("body_gyro")
    if votes is not None and not disable_votes:
        panels.append("apogee_votes")

    if not panels:
        raise SystemExit("error: no telemetry samples in flight window")

    fig, axes = plt.subplots(len(panels), 1, figsize=(12, 2.4 * len(panels)),
                             sharex=True)
    if len(panels) == 1:
        axes = [axes]

    def mark_states(ax, with_labels=False):
        if not transitions:
            return
        ymin, ymax = ax.get_ylim()
        for ts, _frm, to in transitions:
            col = STATE_PALETTE.get(to, c["zero_line"])
            ax.axvline(ts, color=col, linestyle="-", linewidth=1.0, alpha=0.7)
            if with_labels:
                ax.text(ts, ymax, f" {to}", color=col, fontsize=8,
                        va="top", ha="left", rotation=90,
                        bbox=dict(boxstyle="round,pad=0.15",
                                  facecolor=c["legend_bg"],
                                  edgecolor=col, linewidth=0.5, alpha=0.85))

    for ax, panel in zip(axes, panels):
        if panel == "baro":
            t_s, vals = sliced["baro"]
            ax.plot(t_s, vals[:, 0] * 10.0, color=c["pressure"],
                    linewidth=1.2, label="Pressure")
            if gated_mask is not None and np.any(gated_mask):
                idx = np.flatnonzero(gated_mask)
                ax.scatter(t_gated[idx], vals[idx, 0] * 10.0,
                           color=c["apogee"], marker="x", s=28,
                           linewidths=1.2, zorder=5,
                           label=f"NIS-gated (~{len(idx)})")
            ax.set_ylabel("Pressure (hPa)")
            ax.invert_yaxis()
            ax2 = ax.twinx()
            ax2.plot(t_s, vals[:, 1], color=c["velocity"], linewidth=0.8,
                     alpha=0.7, label="Temperature")
            ax2.set_ylabel("Temp (°C)", color=c["velocity"])
            ax2.tick_params(axis="y", colors=c["velocity"])
            ax.legend(loc="upper left")
            ax2.legend(loc="upper right")
            if title is not None:
                ax.set_title(title, color=c["brand"], fontweight="bold")
        elif panel == "sm_pose":
            t_s, vals = sliced["sm_pose"]
            ax.plot(t_s, vals[:, 1], color=c["true_alt"], linewidth=1.5,
                    label="sm_pose.altitude")
            ax.set_ylabel("Altitude (m)")
            ax2 = ax.twinx()
            ax2.plot(t_s, vals[:, 0], color=c["accel_vert"], linewidth=0.8,
                     alpha=0.7, label="sm_pose.orientation")
            ax2.set_ylabel("Orientation (deg)", color=c["accel_vert"])
            ax2.tick_params(axis="y", colors=c["accel_vert"])
            ax.legend(loc="upper left")
            ax2.legend(loc="upper right")
        elif panel == "velocity":
            t_s, vals = sliced["sm_kinematics"]
            ax.plot(t_s, vals[:, 2], color=c["velocity"], linewidth=1.5,
                    label="sm_kinematics.velocity")
            ax.axhline(0, color=c["zero_line"], linewidth=0.5)
            ax.set_ylabel("Velocity (m/s)")
            ax.legend(loc="upper right")
        elif panel == "sm_accel":
            t_s, vals = sliced["sm_kinematics"]
            ax.plot(t_s, vals[:, 0], color=c["g_z"], linewidth=1.0,
                    alpha=0.6, label="|accel|")
            ax.plot(t_s, vals[:, 1], color=c["accel_vert"], linewidth=1.5,
                    label="accel_vert (world)")
            ax.axhline(0, color=c["zero_line"], linewidth=0.5)
            ax.set_ylabel("Acceleration (m/s²)")
            ax.legend(loc="upper right")
        elif panel == "body_accel":
            t_s, vals = sliced["accel"]
            ax.plot(t_s, vals[:, 0], color=c["g_x"], linewidth=0.8, label="a_body[x]")
            ax.plot(t_s, vals[:, 1], color=c["g_y"], linewidth=0.8, label="a_body[y]")
            ax.plot(t_s, vals[:, 2], color=c["g_z"], linewidth=0.8, label="a_body[z]")
            ax.axhline(0, color=c["zero_line"], linewidth=0.5)
            ax.set_ylabel("a_body (m/s²)")
            ax.legend(loc="upper right", ncol=3)
        elif panel == "body_gyro":
            t_s, vals = sliced["gyro"]
            ax.plot(t_s, vals[:, 0], color=c["g_x"], linewidth=0.8, label="ω[x]")
            ax.plot(t_s, vals[:, 1], color=c["g_y"], linewidth=0.8, label="ω[y]")
            ax.plot(t_s, vals[:, 2], color=c["g_z"], linewidth=0.8, label="ω[z]")
            ax.axhline(0, color=c["zero_line"], linewidth=0.5)
            ax.set_ylabel("ω (rad/s)")
            ax.legend(loc="upper right", ncol=3)
        elif panel == "apogee_votes":
            vote_labels = [
                "velocity ≤ 0",
                "altitude < peak",
                "|a_vert| < 20 m/s²",
            ]
            vote_colors = [c["vote_vel"], c["vote_desc"], c["vote_acc"]]
            n_votes = len(vote_labels)
            for row, (label, color) in enumerate(zip(vote_labels,
                                                     vote_colors)):
                base = (n_votes - 1) - row
                ax.fill_between(t_votes, base,
                                base + votes[:, row].astype(float) * 0.8,
                                step="pre", color=color, alpha=0.55,
                                linewidth=0, label=label)
            ax.set_ylabel("Apogee votes")
            ax.set_yticks([0.4 + i for i in range(n_votes)])
            ax.set_yticklabels(list(reversed(vote_labels)))
            ax.set_ylim(-0.1, n_votes)
            ax.legend(loc="upper right", ncol=3, fontsize=8)

        ax.grid(True, color=c["grid"], alpha=0.5)
        mark_states(ax, with_labels=(panel == panels[0]))

    axes[-1].set_xlabel("Time (s)")

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
                    title=(args.title if args.title is not None
                           else "AURORA Kalman Filter - Simulated 500 m Rocket Flight"))


def trim_flight_log(influx_path, windows_ns, out_dir):
    """Write a trimmed copy of ``influx_path`` containing only lines whose
    nanosecond timestamp falls inside any of ``windows_ns`` (a list of
    ``(start_ns, end_ns)`` tuples).

    Influx lines have the timestamp as the final whitespace-separated token.
    Lines whose timestamp cannot be parsed are kept verbatim so that header
    or comment lines pass through unchanged.
    """
    def in_any_window(ts):
        return any(s <= ts <= e for s, e in windows_ns)

    out_influx = os.path.join(out_dir, "flights_trimmed.influx")

    kept = total = 0
    with open(influx_path) as fin, open(out_influx, "w") as fout:
        for line in fin:
            total += 1
            stripped = line.strip()
            if not stripped:
                fout.write(line)
                continue
            try:
                ts = int(stripped.rsplit(" ", 1)[1])
            except (ValueError, IndexError):
                fout.write(line)
                continue
            if in_any_window(ts):
                fout.write(line)
                kept += 1
    print(f"Trimmed influx: kept {kept}/{total} lines -> {out_influx}")


def run_real_flight(args):
    flight_dir = args.flight
    influx_path = os.path.join(flight_dir, "flights.influx")
    audit_path = os.path.join(flight_dir, "state_audit")

    for p in (influx_path, audit_path):
        if not os.path.isfile(p):
            raise SystemExit(f"error: missing {p}")

    print(f"Loading telemetry from {influx_path} ...")
    streams = parse_influx(influx_path)
    for name in FIELD_SPECS:
        print(f"  {name}: {len(streams[name][0])} samples")

    events = parse_state_audit(audit_path)
    flights = segment_flights(events)
    if not flights:
        raise SystemExit("error: no ARMED->BOOST transitions found")
    print(f"Detected {len(flights)} flight(s):")
    for n, (b, e) in enumerate(flights, start=1):
        end_str = f"{e} ns" if e is not None else "end-of-log"
        print(f"  flight {n}: BOOST @ {b} ns → close @ {end_str}")

    if args.trim is not None:
        default_duration_s = 120.0
        pad_ns = int(args.trim * 1000000000)
        windows_ns = []
        for boost_ns, end_ns in flights:
            window_start_ns = boost_ns - int(args.pre_boost * 1000000000)
            raw_end_ns = (end_ns if end_ns is not None
                          else boost_ns + int(default_duration_s * 1000000000))
            window_end_ns = raw_end_ns + int(args.post_end * 1000000000)
            windows_ns.append((window_start_ns - pad_ns,
                               window_end_ns + pad_ns))
        trim_flight_log(influx_path, windows_ns, flight_dir)

    themes = ["light", "dark"] if args.theme == "both" else [args.theme]

    single_flight = len(flights) == 1

    for n, (boost_ns, end_ns) in enumerate(flights, start=1):
        print(f"\n--- Plotting flight {n} ---")
        sliced, transitions = slice_real_flight(
            streams, events, boost_ns, end_ns,
            pre_boost_s=args.pre_boost, post_end_s=args.post_end)

        for name, (t_s, vals) in sliced.items():
            print(f"  {name}: {len(t_s)} samples in window")
        print(f"  transitions: {len(transitions)}")

        if args.title is not None:
            title = args.title
        elif single_flight:
            title = f"AURORA - {os.path.basename(flight_dir.rstrip('/'))}"
        else:
            title = f"AURORA Flight {n} - {os.path.basename(flight_dir.rstrip('/'))}"
        for theme in themes:
            suffix = f"_{theme}" if args.theme == "both" else ""
            out_path = f"flight{n}{suffix}.png"
            plot_raw_flight(sliced, transitions, theme, out_path,
                            title=title, r_meas=args.r_meas, disable_votes=args.disable_votes)


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
        "--disable-votes", action="store_true",
        help="Disable the voting graph.")
    parser.add_argument(
        "--flight", metavar="DIR",
        help="Load real flight data from DIR (expects flights.influx + "
             "state_audit). Produces one plot per flight segmented on "
             "ARMED→BOOST transitions.")
    parser.add_argument(
        "--pre-boost", type=float, default=10.0, dest="pre_boost",
        help="Seconds of pre-boost data to include per flight (default 10)")
    parser.add_argument(
        "--post-end", type=float, default=2.0, dest="post_end",
        help="Seconds of data to keep after the flight close-out "
             "transition (default 2)")
    parser.add_argument(
        "--title", type=str, default=None,
        help="Override the plot title (applies to all generated plots)")
    parser.add_argument(
        "--trim", type=float, nargs="?", const=10.0, default=None,
        metavar="SECONDS",
        help="Write a trimmed copy of flights.influx covering each "
             "flight's plot window expanded by SECONDS on each side "
             "(default 10 s when --trim is given without a value)")
    parser.add_argument(
        "--r-meas", type=float, default=6.0, dest="r_meas",
        help="Assumed Kalman measurement variance (m²) used to "
             "approximate the NIS gate from logged baro vs sm_pose. "
             "Match CONFIG_FILTER_R_MILLISCALE/1000 for the flight "
             "build (default 6.0)")
    args = parser.parse_args()

    if args.flight:
        run_real_flight(args)
    else:
        run_simulation(args)

    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
