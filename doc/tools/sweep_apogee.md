# sweep_apogee.py

Grid-search Kalman filter tuning parameters against recorded flights.

## What it does

`sweep_apogee.py` iterates over a small grid of `(q_vel, r_meas, debounce)`
values and, for each combination, runs every recorded flight in the target
directory through `process_real_flight()` from
[`sim_flight_kalman.py`](sim_flight_kalman.md). For each combination it
records, per flight, the signed delta between the filter-detected apogee
and the ground-truth apogee computed from the log.

A configuration is rejected if:

- the filter fails to detect an apogee on any flight, or
- the filter fires more than 50 ms *before* the ground-truth apogee on any
  flight (no early fires).

Surviving configurations are sorted by worst-case lag across flights and
the top 15 are printed.

## Usage

The script takes no command-line arguments — the flight directory and
search grid are currently hard-coded at the top of the file:

```python
FLIGHT_DIR = AURORA_DIR / "flight_logs/2026-04-18"

grid = list(itertools.product(
    [1.5, 2.0, 3.0, 4.0, 6.0],    # q_vel
    [2.0, 4.0, 6.0, 8.0, 12.0],   # r_meas
    [1, 2],                       # debounce
))
```

Edit those two blocks to point at a different flight capture or to widen
the search space.

Run with:

```bash
python3 tools/sweep_apogee.py
```

Expected output:

```
Sweeping 50 configs

Top 15 configs (lowest worst-case lag, no early fires):
 q_vel     r  db    f1_Δ    f2_Δ   worst
   2.0   4.0   2   +0.08   +0.12   +0.12
   ...
```

- `f1_Δ`, `f2_Δ` — per-flight lag in seconds (positive = late, negative =
  early).
- `worst` — worst-case lag across flights used as the ranking key.

## Input layout

The referenced `FLIGHT_DIR` must contain:

- `flights.influx` — telemetry dump (InfluxDB line protocol).
- `state_audit` — state machine transition audit log.

Both can be produced by [`sim_fetch.py`](sim_fetch.md) or harvested from a
real flight.

## Requirements

- Python 3.10+
- `numpy`
- Must be runnable from the `aurora/tools` directory so the import of
  `sim_flight_kalman` resolves.
