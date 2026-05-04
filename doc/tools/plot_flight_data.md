# plot_flight_data.py

Plot AURORA flight data — recorded telemetry from the flight computer
or simulated streams produced by
[`sim_flight_kalman.py`](sim_flight_kalman.md).

## What it does

This is the dedicated plotting tool for the AURORA flight stack. It
covers two use cases:

- **Standalone CLI:** `python3 plot_flight_data.py --flight DIR`
  segments the log under `DIR` into individual flights on
  `ARMED → BOOST` transitions and produces one multi-panel plot per
  flight, with state-machine transitions drawn as labelled vertical
  markers on every panel. `DIR` is expected to contain
  `flights.influx` (line-protocol telemetry) and `state_audit`
  (state-machine transition log).
- **Imported module:** `sim_flight_kalman.py` imports the module and
  calls `plot_flight(...)` to render the seven-panel plot of its
  simulated output.

The tool does not run the Kalman filter or attitude tracker itself.
What it does carry, on top of pure plotting, is the log-loading and
replay glue needed to turn raw telemetry into the curves that the plot
panels expect — pressure → altitude conversion, NIS-gate replay against
the logged Kalman altitude, the three-vote apogee replay, and the
boost/landed-recovery heuristics for logs whose state machine never
reached `BOOST`.

```{note}
The replay panels (NIS gate, apogee votes) approximate the firmware
behaviour from logged streams alone. The on-board covariance is not
telemetered, so the gate is evaluated under a steady-state assumption.
Treat the markers as a sanity check, not as a perfect re-run.
```

## Usage

```
python3 tools/plot_flight_data.py --flight DIR
                                  [--theme {light,dark,both}]
                                  [--show] [--disable-votes]
                                  [--pre-boost SECONDS]
                                  [--post-end SECONDS]
                                  [--title TITLE]
                                  [--trim [SECONDS]]
                                  [--r-meas R_MEAS]
```

### Data selection

- `--flight DIR` — required; flight directory containing
  `flights.influx` and `state_audit`.
- `--pre-boost SECONDS` — seconds of pre-boost data to keep per flight
  (default `10`).
- `--post-end SECONDS` — seconds of data to keep after the flight
  close-out transition, trimming the long post-parachute tail
  (default `2`).

### Replay

- `--r-meas R_MEAS` — assumed Kalman measurement variance (m²) used to
  approximate the NIS gate from logged baro vs `sm_pose`. Match
  `CONFIG_FILTER_R_MILLISCALE / 1000` for the flight build (default
  `6.0`).
- `--disable-votes` — drop the apogee-vote panel (useful when the
  logged streams are known to be unreliable, e.g. a stuck pointer in
  the logger).

### Output

- `--theme {light,dark,both}` — render plots matching the Furo Sphinx
  docs theme (default `both`).
- `--show` — open the plots in an interactive matplotlib window in
  addition to writing them to disk.
- `--title TITLE` — override the plot title (applied to every plot the
  run produces).
- `--trim [SECONDS]` — write a trimmed copy of `flights.influx` next
  to the original, covering each flight's plot window expanded by
  `SECONDS` on each side (default `10` when `--trim` is given without
  a value). Useful for shrinking long pad-time logs before sharing
  them.

Plots are written to `flight<N>.png` in the current directory, with
`_light` / `_dark` suffixes when `--theme both` is used.

## Reuse from other scripts

The plotting and log-handling helpers are designed to be importable.
The most useful entry points are:

- `parse_influx(path)`, `parse_state_audit(path)`, `segment_flights(events)`
  — load telemetry and split a log into flights.
- `slice_real_flight(...)` — trim raw streams to one flight window.
- `compute_log_votes(sliced)`, `compute_log_gated(sliced, r_meas)` —
  replay the apogee votes and NIS gate from logged streams.
- `altitude_to_pressure(h)` / `pressure_to_altitude(p, p_ref)` — ISA
  conversions.
- `plot_flight(...)` — seven-panel plot used by
  [`sim_flight_kalman.py`](sim_flight_kalman.md).
- `plot_raw_flight(...)` — multi-panel raw-telemetry plot used by the
  CLI above.

## Requirements

- Python 3.10+
- `numpy`, `matplotlib`
