#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Plotting tool for AURORA flight data.

This module is dedicated to rendering plots of flight data — recorded
telemetry from the flight computer or simulated streams produced by
``sim_flight_kalman.py``.  It is *only* used to plot flight data; it does
not run the Kalman filter or attitude tracker itself, but it does carry the
log-loading / event-segmentation / replay helpers needed to reduce raw
telemetry into the curves and markers that the plot panels expect.

Two entry points are exposed:

* :func:`plot_flight` — 7-panel plot of a fully-resolved flight record
  (pressure, altitude, velocity, body gravity, gyro, world-vertical accel,
  apogee votes).  Used by ``sim_flight_kalman.py`` to plot the simulator
  output, and could equally be driven from any other source that produces
  the same per-sample arrays.
* :func:`plot_raw_flight` — multi-panel plot of recorded telemetry sliced
  to one flight window, with state-machine transitions marked.

Run as a script with ``--flight DIR`` to plot recorded telemetry directly:
``DIR`` must contain ``flights.influx`` (line-protocol telemetry) and
``state_audit`` (state-machine transitions).
"""

import argparse
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
G0 = 9.80665    # Gravitational acceleration (m/s^2)
M  = 0.0289644  # Molar mass of dry air (kg/mol)
R_GAS = 8.31447 # Universal gas constant (J/(mol·K))


def altitude_to_pressure(h):
    """ISA barometric formula: altitude (m) -> pressure (Pa)."""
    return P0 * (1 - L * h / T0) ** (G0 * M / (R_GAS * L))


def pressure_to_altitude(p, p_ref=P0):
    """Hypsometric formula: pressure (Pa) -> altitude (m) AGL.

    Uses a ground-level reference pressure ``p_ref`` so that the returned
    altitude is 0 m at launch.
    """
    return (T0 / L) * (1 - (p / p_ref) ** (R_GAS * L / (G0 * M)))


# ---------------------------------------------------------------------------
# Replay constants — must mirror the firmware values in kalman.c
# ---------------------------------------------------------------------------

# NIS (Mahalanobis) gate: ~5-sigma.  Used to flag baro samples that the
# firmware's filter would have rejected as glitches.
FILTER_NIS_GATE = 25.0
FILTER_NIS_WARMUP = 30
FILTER_APOGEE_ACCEL_BAND = 20.0


# ---------------------------------------------------------------------------
# Real flight log parsing (InfluxDB line protocol + state_audit)
# ---------------------------------------------------------------------------

FIELD_SPECS = {
    "accel":         ("x", "y", "z"),
    "gyro":          ("x", "y", "z"),
    "baro":          ("pres", "temp"),
    "sm_kinematics": ("accel", "accel_vert"),
    "sm_pose":       ("velocity", "altitude"),
    "orientation":   ("yaw", "pitch", "roll"),
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


def _first_sustained_run(t_ns, mask, sustain):
    """Return ``t_ns`` at the first index where ``mask`` is True for
    ``sustain`` consecutive samples, or ``None`` if no such run exists."""
    run = 0
    for i, hit in enumerate(mask):
        if hit:
            run += 1
            if run >= sustain:
                return int(t_ns[i - sustain + 1])
        else:
            run = 0
    return None


def _samples_for_seconds(t_ns, seconds):
    """Convert a duration in seconds to a sample count using the median
    inter-sample interval of ``t_ns``. Falls back to 1 if the timeline
    is too short to estimate."""
    if t_ns.size < 2:
        return 1
    dt_ns = float(np.median(np.diff(t_ns)))
    if dt_ns <= 0:
        return 1
    return max(1, int(seconds * 1e9 / dt_ns))


def recover_boost_from_measurements(streams, accel_threshold=15.0,
                                    baro_climb_threshold=5.0, sustain=5):
    """Heuristically locate boost ignition from raw measurements.

    Used as a fallback when the state_audit log contains no ARMED→BOOST
    transition. Returns ``(boost_ns, source_label)`` or ``(None, None)`` if
    no source yielded a usable signal.

    Sources tried, in order:
      1. raw ``accel`` body stream — first run of ``sustain`` samples with
         ``|a_body|`` above ``accel_threshold`` m/s²;
      2. ``sm_kinematics.accel`` — same threshold against the on-board
         ``|a|`` magnitude column when the raw stream is empty;
      3. ``baro`` — climb rate derived from a 2 s sliding altitude
         difference, first run above ``baro_climb_threshold`` m/s.
    """
    t_ns, vals = streams["accel"]
    if t_ns.size:
        mag = np.linalg.norm(vals, axis=1)
        boost = _first_sustained_run(t_ns, mag > accel_threshold, sustain)
        if boost is not None:
            return boost, "|a_body|"

    t_ns, vals = streams["sm_kinematics"]
    if t_ns.size:
        boost = _first_sustained_run(t_ns, vals[:, 0] > accel_threshold,
                                     sustain)
        if boost is not None:
            return boost, "sm_kinematics.accel"

    t_ns, vals = streams["baro"]
    if t_ns.size > 1:
        p_pa = vals[:, 0] * 1000.0
        alt = pressure_to_altitude(p_pa, p_pa[0])
        win = _samples_for_seconds(t_ns, 2.0)
        if alt.size > win:
            dt_s = (t_ns[win:] - t_ns[:-win]) / 1e9
            rate = np.divide(alt[win:] - alt[:-win], dt_s,
                             out=np.zeros_like(dt_s), where=dt_s > 0)
            boost = _first_sustained_run(t_ns[win:],
                                         rate > baro_climb_threshold,
                                         sustain)
            if boost is not None:
                return boost, "baro climb rate"

    return None, None


def recover_landed_from_measurements(streams, boost_ns,
                                     baro_alt_threshold=10.0,
                                     accel_band=2.0, quiet_seconds=5.0):
    """Heuristically locate touchdown after a recovered boost."""
    t_ns, vals = streams["baro"]
    if t_ns.size:
        p_pa = vals[:, 0] * 1000.0
        alt = pressure_to_altitude(p_pa, p_pa[0])
        post = t_ns >= boost_ns
        if np.any(post):
            t_post = t_ns[post]
            sustain = _samples_for_seconds(t_post, quiet_seconds)
            ts = _first_sustained_run(t_post,
                                      alt[post] < baro_alt_threshold,
                                      sustain)
            if ts is not None:
                return ts, "baro altitude"

    t_ns, vals = streams["accel"]
    if t_ns.size:
        post = t_ns >= boost_ns
        if np.any(post):
            t_post = t_ns[post]
            mag = np.linalg.norm(vals[post], axis=1)
            sustain = _samples_for_seconds(t_post, quiet_seconds)
            ts = _first_sustained_run(t_post,
                                      np.abs(mag - G0) < accel_band,
                                      sustain)
            if ts is not None:
                return ts, "|a_body| quiescence"

    return None, None


def slice_real_flight(streams, events, boost_ns, end_ns,
                      pre_boost_s=10.0, post_end_s=2.0,
                      default_duration_s=120.0, relax_empty=False):
    """Slice the raw telemetry streams to a single flight window.

    The window starts ``pre_boost_s`` seconds before the BOOST transition
    and ends ``post_end_s`` seconds after the flight close-out (or after
    ``default_duration_s`` if the audit log never closes the flight). All
    timestamps are returned in seconds, with t=0 at the window start so
    the BOOST transition lands at t = ``pre_boost_s``.

    When ``relax_empty`` is set, every stream falls back to its full
    timeline rather than being masked to the window.
    """
    window_start_ns = boost_ns - int(pre_boost_s * 1000000000)
    raw_end_ns = (end_ns if end_ns is not None
                  else boost_ns + int(default_duration_s * 1000000000))
    window_end_ns = raw_end_ns + int(post_end_s * 1000000000)

    sliced = {}
    for name, (t_ns_raw, vals_raw) in streams.items():
        if relax_empty:
            t_s = (t_ns_raw - window_start_ns) / 1000000000.0
            sliced[name] = (t_s, vals_raw)
        else:
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
    columns matching ``filter_detect_apogee`` — velocity ≤ 0, altitude <
    running peak, |accel_vert| < FILTER_APOGEE_ACCEL_BAND.  Returns
    ``(None, None)`` if either stream is empty.
    """
    t_kin, kin = sliced["sm_kinematics"]
    t_pose, pose = sliced["sm_pose"]
    if t_kin.size == 0 or t_pose.size == 0:
        return None, None
    altitude = np.interp(t_kin, t_pose, pose[:, 1])
    velocity = np.interp(t_kin, t_pose, pose[:, 0])
    accel_vert = kin[:, 1]
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
    threshold.
    """
    t_baro, baro = sliced["baro"]
    t_pose, pose = sliced["sm_pose"]
    if t_baro.size == 0 or t_pose.size == 0:
        return None, None, None
    p_pa = baro[:, 0] * 1000.0
    baro_alt = pressure_to_altitude(p_pa, p_pa[0])
    kalman_alt = np.interp(t_baro, t_pose, pose[:, 1])
    y = baro_alt - kalman_alt
    gated = (y * y) > FILTER_NIS_GATE * r_meas
    if warmup > 0:
        gated[:warmup] = False
    return t_baro, gated, baro_alt


def trim_flight_log(influx_path, windows_ns, out_dir):
    """Write a trimmed copy of ``influx_path`` containing only lines whose
    nanosecond timestamp falls inside any of ``windows_ns`` (a list of
    ``(start_ns, end_ns)`` tuples)."""
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
    """Generate the 7-panel flight plot using the given theme.

    Optional overlays:
      * ``true_alt`` / ``true_press`` / ``true_vel`` — ground-truth reference
        curves (only available for the simulation path).
      * ``state_transitions`` — list of ``(t_s, from_state, to_state)``
        tuples drawn as labelled vertical lines on every panel.
      * ``computed_apogee_idx`` — index of the filtered-altitude peak,
        drawn as a thin reference line for comparison with ``apogee_idx``.
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
            col = STATE_PALETTE.get(to, c["zero_line"])
            if with_dt_apogee and to == "APOGEE":
                dt_apogee = ts - t[computed_apogee_idx]
                label = (f"Apogee detected (flight) (t={ts:.2f} s, "
                         f"Δ={dt_apogee:+.2f} s)")
                print(label)
                ax.axvline(ts, color=col, linestyle="-", linewidth=1.0,
                           alpha=0.7, label=label)
            else:
                ax.axvline(ts, color=col, linestyle="-", linewidth=1.0,
                           alpha=0.7)
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
    peak_idx = int(np.argmax(filtered_alt))
    peak_alt = float(filtered_alt[peak_idx])
    peak_t = float(t[peak_idx])
    ax_alt.axhline(peak_alt, color=c["fg"], linestyle=":", linewidth=0.8,
                   alpha=0.4,
                   label=f"Peak altitude ({peak_alt:.1f} m @ {peak_t:.2f} s)")
    ax_alt.axvline(peak_t, color=c["fg"], linestyle=":", linewidth=0.8,
                   alpha=0.4)
    if computed_apogee_idx is not None:
        mark_computed_apogee(
            ax_alt,
            f"Computed apogee (t={t[computed_apogee_idx]:.2f} s)")
    if apogee_idx is not None:
        if computed_apogee_idx is not None:
            dt_apogee = t[apogee_idx] - t[computed_apogee_idx]
            apogee_label = (f"Apogee detected (simulator) "
                            f"(t={t[apogee_idx]:.2f} s, Δ={dt_apogee:+.2f} s)")
        else:
            apogee_label = (f"Apogee detected (simulator) "
                            f"(t={t[apogee_idx]:.2f} s)")
        mark_apogee(ax_alt, apogee_label)
        ax_alt.plot(t[apogee_idx], filtered_alt[apogee_idx], "o",
                    color=c["apogee"], markersize=8)
    ax_alt.set_ylabel("Altitude (m)")
    ax_alt.grid(True, color=c["grid"], alpha=0.5)
    mark_states(ax_alt, with_dt_apogee=True)
    ax_alt.legend(loc="upper right")

    # 3) Vertical velocity
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

    # 6) Gravity-removed world-vertical accel
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

    # 7) Apogee votes
    vote_labels = [
        "velocity <= -k·σ_vel",
        "altitude < peak",
        "|a_vert| < 20 m/s²",
    ]
    vote_colors = [c["vote_vel"], c["vote_desc"], c["vote_acc"]]
    n_votes = len(vote_labels)
    for row, (label, color) in enumerate(zip(vote_labels, vote_colors)):
        base = (n_votes - 1) - row
        ax_vote.fill_between(t, base,
                             base + vote_hist[:, row].astype(float) * 0.8,
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
        panels.append("velocity")
    if sliced["sm_kinematics"][0].size:
        panels.append("sm_accel")
    if sliced["accel"][0].size:
        panels.append("body_accel")
    if sliced["gyro"][0].size:
        panels.append("body_gyro")
    if sliced["orientation"][0].size:
        panels.append("orientation")
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
            if vals[:, 1].size:
                peak_idx = int(np.argmax(vals[:, 1]))
                peak_alt = float(vals[peak_idx, 1])
                peak_t = float(t_s[peak_idx])
                ax.axhline(peak_alt, color=c["fg"], linestyle=":",
                           linewidth=0.8, alpha=0.4,
                           label=f"Peak altitude ({peak_alt:.1f} m @ "
                                 f"{peak_t:.2f} s)")
                ax.axvline(peak_t, color=c["fg"], linestyle=":",
                           linewidth=0.8, alpha=0.4)
            ax.set_ylabel("Altitude (m)")
            ax.legend(loc="upper left")
        elif panel == "velocity":
            t_s, vals = sliced["sm_pose"]
            ax.plot(t_s, vals[:, 0], color=c["velocity"], linewidth=1.5,
                    label="sm_pose.velocity")
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
            ax.plot(t_s, vals[:, 0], color=c["g_x"], linewidth=0.8,
                    label="a_body[x]")
            ax.plot(t_s, vals[:, 1], color=c["g_y"], linewidth=0.8,
                    label="a_body[y]")
            ax.plot(t_s, vals[:, 2], color=c["g_z"], linewidth=0.8,
                    label="a_body[z]")
            ax.axhline(0, color=c["zero_line"], linewidth=0.5)
            ax.set_ylabel("a_body (m/s²)")
            ax.legend(loc="upper right", ncol=3)
        elif panel == "body_gyro":
            t_s, vals = sliced["gyro"]
            ax.plot(t_s, vals[:, 0], color=c["g_x"], linewidth=0.8,
                    label="ω[x]")
            ax.plot(t_s, vals[:, 1], color=c["g_y"], linewidth=0.8,
                    label="ω[y]")
            ax.plot(t_s, vals[:, 2], color=c["g_z"], linewidth=0.8,
                    label="ω[z]")
            ax.axhline(0, color=c["zero_line"], linewidth=0.5)
            ax.set_ylabel("ω (rad/s)")
            ax.legend(loc="upper right", ncol=3)
        elif panel == "orientation":
            t_s, vals = sliced["orientation"]
            ax.plot(t_s, vals[:, 0], color=c["g_x"], linewidth=1.0,
                    label="yaw")
            ax.plot(t_s, vals[:, 1], color=c["g_y"], linewidth=1.0,
                    label="pitch")
            ax.plot(t_s, vals[:, 2], color=c["g_z"], linewidth=1.0,
                    label="roll")
            ax.axhline(0, color=c["zero_line"], linewidth=0.5)
            ax.set_ylabel("Orientation (deg)")
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
# Main: plot recorded telemetry from a flight directory
# ---------------------------------------------------------------------------

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
    recovered = False
    if not flights:
        print("warning: no ARMED->BOOST transition found in state_audit; "
              "attempting to recover flight from raw |a_body|")
        boost_ns, boost_src = recover_boost_from_measurements(streams)
        if boost_ns is None:
            print("  no boost-like signal found; plotting entire log")
            first_ns = min((t[0] for t, _ in streams.values() if len(t)),
                           default=0)
            flights = [(first_ns, None)]
            recovered = True
        else:
            print(f"  recovered boost  @ {boost_ns} ns (from {boost_src})")
            landed_ns, landed_src = recover_landed_from_measurements(
                streams, boost_ns)
            if landed_ns is not None:
                print(f"  recovered landed @ {landed_ns} ns "
                      f"(from {landed_src})")
            else:
                print("  recovered landed: not found, falling back to "
                      "default post-boost duration")
            events.append((boost_ns, "transition", "ARMED", "BOOST"))
            if landed_ns is not None:
                events.append((landed_ns, "transition", "FLIGHT", "LANDED"))
            events.sort(key=lambda e: e[0])
            flights = [(boost_ns, landed_ns)]
            recovered = True
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
                          else boost_ns + int(default_duration_s
                                              * 1000000000))
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
            pre_boost_s=args.pre_boost, post_end_s=args.post_end,
            relax_empty=recovered)

        for name, (t_s, vals) in sliced.items():
            print(f"  {name}: {len(t_s)} samples in window")
        print(f"  transitions: {len(transitions)}")

        if args.title is not None:
            title = args.title
        elif single_flight:
            title = f"AURORA - {os.path.basename(flight_dir.rstrip('/'))}"
        else:
            title = (f"AURORA Flight {n} - "
                     f"{os.path.basename(flight_dir.rstrip('/'))}")
        if recovered:
            title += "  (boost recovered from accel — no BOOST transition)"
        for theme in themes:
            suffix = f"_{theme}" if args.theme == "both" else ""
            out_path = f"flight{n}{suffix}.png"
            plot_raw_flight(sliced, transitions, theme, out_path,
                            title=title, r_meas=args.r_meas,
                            disable_votes=args.disable_votes)


def main():
    parser = argparse.ArgumentParser(
        description="Plot AURORA flight data from recorded telemetry logs.")
    parser.add_argument(
        "--flight", metavar="DIR", required=True,
        help="Load real flight data from DIR (expects flights.influx + "
             "state_audit). Produces one plot per flight segmented on "
             "ARMED→BOOST transitions.")
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

    run_real_flight(args)

    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
