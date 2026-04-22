# sim_flight_kalman.py

Replay a simulated or recorded rocket flight through a Python mirror of the
AURORA Kalman filter and inspect the result graphically.

## What it does

The script replicates the two-state (altitude, velocity) constant-
acceleration Kalman filter from [`aurora/lib/filter/kalman.c`](https://github.com/AUXSPACEeV/aurora/tree/main/lib/filter/kalman.c)
together with the body-frame gravity tracker from
[`aurora/lib/sensor/attitude.c`](https://github.com/AUXSPACEeV/aurora/tree/main/lib/sensor/attitude.c),
so that filter tuning and apogee detection can be analysed offline without
flashing firmware.

Two data sources are supported:

- **Synthetic flight (default):** a pressure signal is generated from the
  ISA barometric formula and a body-frame IMU stream (accel + gyro) is
  synthesized with a slow pitch-over after apogee.
- **Recorded flight (`--flight DIR`):** the script loads telemetry from
  `DIR/flights.influx` and the state machine trace from `DIR/state_audit`,
  segments the log into individual flights on `ARMED → BOOST` transitions,
  and produces one plot per flight with state transitions drawn as vertical
  markers.

Plots cover pressure, altitude, vertical velocity, the body-frame gravity
direction, the gravity-removed world-vertical acceleration, and the
three-way apogee vote.

## Usage

```
python3 tools/sim_flight_kalman.py [--flight DIR] [--show]
                                   [--theme {light,dark,both}]
                                   [--dt S] [--pre-boost S] [--cal S]
                                   [--post-main S]
                                   [--q-alt Q] [--q-vel Q] [--r-meas R]
                                   [--debounce N]
```

### Data selection

- `--flight DIR` — load real flight data from `DIR` (expects
  `flights.influx` and `state_audit`). Without this flag, a synthetic
  flight is generated.
- `--dt` — resampling period in seconds for real flight data (default
  `0.02`).
- `--pre-boost` — seconds of pre-boost data to keep per flight (default
  `10`).
- `--cal` — length of the calibration window before BOOST (default `3`).
- `--post-main` — seconds of data to keep after the `MAIN` transition,
  trimming the long post-parachute tail (default `5`).

### Kalman tuning

The defaults match the values committed in Kconfig so runs are directly
comparable to firmware behaviour.

- `--q-alt` — process noise on altitude (default `0.1`).
- `--q-vel` — process noise on velocity (default `0.5`).
- `--r-meas` — measurement noise variance (default `4.0`).
- `--debounce` — apogee debounce ticks (default `3`).

### Output

- `--theme {light,dark,both}` — render plots matching the Furo Sphinx docs
  theme (default `both`).
- `--show` — open the plots in an interactive matplotlib window in
  addition to writing them to disk.

On run, the script produces a graph and saves it to a file called
`flight_simulation.png`. Here is an example run:

```bash
Plot saved to flight_simulation.png
Ground ref pressure: 101340 Pa (1013.4 hPa)
True apogee:         500.0 m
Filter apogee:       496.4 m (t = 12.56 s)
```

```{image} /img/flight_simulation_light.png
:alt: flight_simulation_light.png
:class: only-light
```

```{image} /img/flight_simulation_dark.png
:alt: flight_simulation_dark.png
:class: only-dark
```

## Reuse from other scripts

`sim_flight_kalman.py` also exposes the helpers used by
[`sweep_apogee.py`](sweep_apogee.md):

- `parse_influx(path)` — parse a recorded `flights.influx` telemetry dump.
- `parse_state_audit(path)` — parse a state-machine audit log.
- `segment_flights(events)` — split a trace into `(boost_ms, end_ms)`
  ranges on `ARMED → BOOST` transitions.
- `process_real_flight(...)` — run one flight through the Python filter and
  return a dict of time series plus detected apogee indices.

## Requirements

- Python 3.10+
- `numpy`, `matplotlib`
