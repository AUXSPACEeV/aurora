#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Simulated rocket flight through the AURORA Kalman filter.

Replicates the 2-state (altitude, velocity) constant-velocity Kalman filter
from aurora/lib/apogee/kalman.c.
The simulation generates a realistic pressure signal from the ISA barometric
formula, adds sensor noise in the pressure domain, then converts back to
altitude via the hypsometric formula before feeding the Kalman filter, matching
the real sensor pipeline.
"""

import argparse
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

class KalmanFilter:
    """2-state Kalman filter identical to struct filter / kalman.c."""

    def __init__(self, q_alt=0.1, q_vel=0.5, r_meas=4.0):
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

        self.prev_velocity = 0.0

    def predict(self, dt_s: float):
        """filter_predict - constant-velocity state propagation."""
        self.state[0] += self.state[1] * dt_s

        P = self.P
        Q00 = self.Q[0, 0] * dt_s
        Q11 = self.Q[1, 1] * dt_s

        new_P = np.zeros((2, 2))
        new_P[0, 0] = P[0, 0] + dt_s * (P[1, 0] + P[0, 1]) + dt_s**2 * P[1, 1] + Q00
        new_P[0, 1] = P[0, 1] + dt_s * P[1, 1]
        new_P[1, 0] = P[1, 0] + dt_s * P[1, 1]
        new_P[1, 1] = P[1, 1] + Q11
        self.P = new_P

    def update(self, z: float):
        """filter_update - measurement correction."""
        y = z - self.state[0]
        S = self.P[0, 0] + self.R

        K0 = self.P[0, 0] / S
        K1 = self.P[1, 0] / S

        self.state[0] += K0 * y
        self.state[1] += K1 * y

        P = self.P.copy()
        self.P[0, 0] = P[0, 0] - K0 * P[0, 0]
        self.P[0, 1] = P[0, 1] - K0 * P[0, 1]
        self.P[1, 0] = P[1, 0] - K1 * P[0, 0]
        self.P[1, 1] = P[1, 1] - K1 * P[0, 1]

    def detect_apogee(self) -> bool:
        """filter_detect_apogee - velocity zero-crossing."""
        current = self.state[1]
        apogee = self.prev_velocity > 0.0 and current <= 0.0
        self.prev_velocity = current
        return apogee


# ---------------------------------------------------------------------------
# Flight simulation
# ---------------------------------------------------------------------------

def simulate_flight(apogee_m=500.0, dt=0.02, seed=42):
    """
    Generate a realistic rocket altitude profile and barometric pressure.

    Phases:
      1. Boost   - constant thrust acceleration for ~3 s
      2. Coast   - ballistic arc (gravity only) up to apogee
      3. Descent - drogue then main chute

    Returns (time, true_altitude, true_pressure, noisy_pressure) arrays.
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
    actual_apogee = h_burnout + v_burnout * coast_time - 0.5 * g * coast_time**2

    # --- Descent phase ---
    drogue_v = 30.0   # m/s under drogue
    main_deploy_h = 150.0
    main_v = 6.0      # m/s under main

    descent_time_drogue = (actual_apogee - main_deploy_h) / drogue_v
    descent_time_main = main_deploy_h / main_v
    total_time = burn_time + coast_time + descent_time_drogue + descent_time_main + 2.0

    t = np.arange(0, total_time, dt)
    altitude = np.zeros_like(t)

    for i, ti in enumerate(t):
        if ti <= burn_time:
            altitude[i] = 0.5 * a_thrust * ti**2
        elif ti <= burn_time + coast_time:
            tc = ti - burn_time
            altitude[i] = h_burnout + v_burnout * tc - 0.5 * g * tc**2
        elif ti <= burn_time + coast_time + descent_time_drogue:
            td = ti - burn_time - coast_time
            altitude[i] = actual_apogee - drogue_v * td
        elif ti <= burn_time + coast_time + descent_time_drogue + descent_time_main:
            tm = ti - burn_time - coast_time - descent_time_drogue
            altitude[i] = main_deploy_h - main_v * tm
        else:
            altitude[i] = 0.0

    altitude = np.maximum(altitude, 0.0)

    # --- Pressure domain ---
    true_pressure = altitude_to_pressure(altitude)

    # MS5607 noise: ~50 Pa RMS at 4096 OSR (datasheet typical)
    pressure_noise = rng.normal(0.0, 50.0, size=len(t))
    noisy_pressure = true_pressure + pressure_noise

    return t, altitude, true_pressure, noisy_pressure


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
        "apogee":       "#c01c28",
        "zero_line":    "#333333",
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
        "apogee":       "#ff6b6b",
        "zero_line":    "#8890a0",
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

def plot_flight(t, true_alt, true_press, noisy_press, baro_alt,
                filtered_alt, filtered_vel, apogee_idx, theme_name, out_path):
    """Generate the 3-panel flight plot using the given theme."""
    c = apply_theme(theme_name)

    fig, (ax_press, ax_alt, ax_vel) = plt.subplots(
        3, 1, figsize=(12, 10), sharex=True,
        gridspec_kw={"height_ratios": [2, 3, 1]},
    )

    # Top: raw pressure
    ax_press.plot(t, noisy_press / 100, color=c["noisy"], linewidth=0.5,
                  label="Noisy baro reading")
    ax_press.plot(t, true_press / 100, color=c["pressure"], linewidth=1.5,
                  label="True pressure")
    if apogee_idx is not None:
        ax_press.axvline(t[apogee_idx], color=c["apogee"],
                         linestyle="--", linewidth=1.5)
    ax_press.set_ylabel("Pressure (hPa)")
    ax_press.set_title("AURORA Kalman Filter - Simulated 500 m Rocket Flight",
                       color=c["brand"], fontweight="bold")
    ax_press.legend(loc="upper right")
    ax_press.grid(True, color=c["grid"], alpha=0.5)
    ax_press.invert_yaxis()

    # Middle: altitude
    ax_alt.plot(t, baro_alt, color=c["noisy"], linewidth=0.5,
                label="Baro altitude (hypsometric)")
    ax_alt.plot(t, true_alt, color=c["true_alt"], linewidth=2,
                label="True altitude")
    ax_alt.plot(t, filtered_alt, color=c["filtered"], linewidth=2,
                label="Kalman filter output")
    if apogee_idx is not None:
        ax_alt.axvline(t[apogee_idx], color=c["apogee"], linestyle="--",
                       linewidth=1.5,
                       label=f"Apogee detected (t={t[apogee_idx]:.2f} s)")
        ax_alt.plot(t[apogee_idx], filtered_alt[apogee_idx], "o",
                    color=c["apogee"], markersize=8)
    ax_alt.set_ylabel("Altitude (m)")
    ax_alt.legend(loc="upper right")
    ax_alt.grid(True, color=c["grid"], alpha=0.5)

    # Bottom: velocity
    ax_vel.plot(t, filtered_vel, color=c["velocity"], linewidth=1.5,
                label="Estimated velocity")
    ax_vel.axhline(0, color=c["zero_line"], linewidth=0.5)
    if apogee_idx is not None:
        ax_vel.axvline(t[apogee_idx], color=c["apogee"],
                       linestyle="--", linewidth=1.5)
    ax_vel.set_xlabel("Time (s)")
    ax_vel.set_ylabel("Velocity (m/s)")
    ax_vel.legend(loc="upper right")
    ax_vel.grid(True, color=c["grid"], alpha=0.5)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"[{theme_name}] Plot saved to {out_path}")
    return fig


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Simulated rocket flight through the AURORA Kalman filter.")
    parser.add_argument(
        "--theme", choices=["light", "dark", "both"], default="both",
        help="Colour theme matching the Furo Sphinx docs (default: both)")
    parser.add_argument(
        "--show", action="store_true",
        help="Open an interactive matplotlib window")
    args = parser.parse_args()

    dt = 0.02  # 50 Hz sample rate
    t, true_alt, true_press, noisy_press = simulate_flight(apogee_m=500.0, dt=dt)

    # Ground-level reference pressure (first sample, before launch)
    p_ref = noisy_press[0]

    # Convert noisy pressure -> altitude via hypsometric formula
    baro_alt = pressure_to_altitude(noisy_press, p_ref)

    kf = KalmanFilter(q_alt=0.1, q_vel=0.5, r_meas=25.0)

    filtered_alt = np.zeros_like(t)
    filtered_vel = np.zeros_like(t)
    apogee_idx = None

    # Skip apogee detection for the first second to let the filter settle
    settle_samples = int(1.0 / dt)

    for i in range(len(t)):
        kf.predict(dt)
        kf.update(baro_alt[i])
        filtered_alt[i] = kf.state[0]
        filtered_vel[i] = kf.state[1]
        if apogee_idx is None and i > settle_samples and kf.detect_apogee():
            apogee_idx = i

    print(f"Ground ref pressure: {p_ref:.0f} Pa ({p_ref/100:.1f} hPa)")
    print(f"True apogee:         {true_alt.max():.1f} m")
    print(f"Filter apogee:       {filtered_alt[apogee_idx]:.1f} m"
          f" (t = {t[apogee_idx]:.2f} s)")

    themes = ["light", "dark"] if args.theme == "both" else [args.theme]
    for theme in themes:
        suffix = f"_{theme}" if args.theme == "both" else ""
        out_path = f"flight_simulation{suffix}.png"
        plot_flight(t, true_alt, true_press, noisy_press, baro_alt,
                    filtered_alt, filtered_vel, apogee_idx, theme, out_path)

    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
