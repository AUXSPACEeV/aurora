#!/usr/bin/env python3
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""Refined grid search around the fast region found in coarse sweep."""

import itertools
import os
import numpy as np
import pathlib

from plot_flight_data import (parse_influx, parse_state_audit,
                              segment_flights)
from sim_flight_kalman import process_real_flight

AURORA_DIR = pathlib.Path(os.path.dirname(os.path.abspath(__file__))).parent
FLIGHT_DIR = AURORA_DIR / "flight_logs/2026-04-18"

streams = parse_influx(FLIGHT_DIR / "flights.influx")
events = parse_state_audit(FLIGHT_DIR / "state_audit")
flights = segment_flights(events)

grid = list(itertools.product(
    [1.5, 2.0, 3.0, 4.0, 6.0],    # q_vel
    [2.0, 4.0, 6.0, 8.0, 12.0],   # r_meas
    [1, 2],                       # debounce
))

print(f"Sweeping {len(grid)} configs")
results = []
for q_vel, r_meas, debounce in grid:
    deltas = []
    valid = True
    for boost_ms, end_ms in flights:
        res = process_real_flight(streams, events, boost_ms, end_ms,
                                  dt_s=0.02, pre_boost_s=10.0, cal_s=3.0,
                                  post_main_s=5.0,
                                  q_alt=0.1, q_vel=q_vel, r_meas=r_meas,
                                  apogee_debounce=debounce)
        apogee_idx = res["apogee_idx"]
        actual_idx = res["computed_apogee_idx"]
        if apogee_idx is None:
            valid = False
            deltas.append(float("nan"))
            continue
        t = res["t"]
        if t[apogee_idx] < t[actual_idx] - 0.05:
            valid = False
        deltas.append(t[apogee_idx] - t[actual_idx])

    f1, f2 = deltas
    worst = (max(d for d in deltas if not np.isnan(d)) if valid
             else float("inf"))
    results.append((q_vel, r_meas, debounce, f1, f2, worst, valid))

valid_results = [r for r in results if r[6]]
valid_results.sort(key=lambda r: r[5])
print(f"\nTop 15 configs (lowest worst-case lag, no early fires):")
print(f"{'q_vel':>6} {'r':>5} {'kσ':>5} {'db':>3} "
      f"{'f1_Δ':>7} {'f2_Δ':>7} {'worst':>7}")
for q_vel, r_meas, debounce, f1, f2, worst, _ in valid_results[:15]:
    print(f"{q_vel:>6.1f} {r_meas:>5.1f} {debounce:>3d} "
          f"{f1:>+7.2f} {f2:>+7.2f} {worst:>+7.2f}")
