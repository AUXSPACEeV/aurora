# Architecture

AURORA runs on multiple interconnected PCBs that communicate over CAN bus.
The primary application is `sensor_board`, which manages the IMU, barometric
sensor, flight state machine, and pyrotechnic ignition.

## Threads

`sensor_board` uses three Zephyr threads (all priority 5, 2 KB stack):

| Thread | Guard | Purpose |
|---|---|---|
| `imu_task` | `CONFIG_IMU` | Polls IMU at the configured frequency, updates orientation and acceleration globals. |
| `baro_task` | `CONFIG_BARO` | Measures pressure/temperature, computes altitude. |
| `state_machine_task` | `CONFIG_AURORA_STATE_MACHINE` | Runs at 10 Hz. Feeds sensor data into the state machine and fires pyro channels on state transitions. |

## Sensor Data Path

It's a tricky situation trying to fetch data from all sensors at the exact same
time and passing them on to the state machine, the apogee filer and the data
logger.
Here is a rough overview of how AURORA's data is passed between threads to the
state machine and the data logger:

![sensors datapath](img/sensor_datapath.drawio.svg)

## State Machine

AURORA features a dynamic selection of state machine types via Kconfig
`CONFIG_AURORA_STATE_MACHINE_TYPE`.
Currently only the simple state machine is implemented and it uses the
following flight sequence:

![states](img/aurora_simple_state_machine.drawio.svg)

| Signal | Comment |
|---|---|
| ARM | *ARM* signal from extern. Arms the pyro channels as well |
| DISARM | *DISARM* signal from extern. Disarms the pyro channels as well |

| Sensor Reading | Comment |
|---|---|
| T{sub}`AB` | Acceleration needed to go from ARMED to BOOST |
| T{sub}`H` | Altitude needed to go from ARMED to BOOST |
| T{sub}`BB` | Acceleratioin needed to go from BOOST to BURNOUT |
| T{sub}`M` | Altitude needed to go from APOGEE to MAIN |
| T{sub}`L` | Velocity needed to go signal LANDED |

| Timer | Comment |
|---|---|
| DT{sub}`AB` | Time that T_AB and T_H shall be asserted for |
| DT{sub}`L` | Time that T_L shall be asserted |

| Timeout | Comment |
|---|---|
| TO{sub}`A` | Timeout for APOGEE state |
| TO{sub}`R` | Timeout for REDUNDAND state |

State transitions are also driven by sensor thresholds configured via Kconfig
(boost acceleration, main descent height, apogee timeout, etc.).

Other state machines may come soon, e.g. multi-stage with two boosting phases.

## Apogee detection

AURORA detects apogee using a scalar constant-velocity Kalman filter
(guarded by `CONFIG_APOGEE_DETECTION` and `CONFIG_FILTER_KALMAN`).
The filter tracks a two-element state vector — altitude and vertical
velocity — and is fed barometric altitude measurements that the baro
library converts from pressure via the ISA hypsometric formula.

On every state-machine tick the filter runs a predict/update cycle:

- The predict step propagates altitude forward using the current velocity
estimate while growing the covariance by a process-noise term scaled
with dt
- The update step corrects the state with the latest barometric
altitude reading through the standard Kalman gain.

Apogee is declared the moment the estimated velocity crosses zero from
positive to non-positive, which in turn triggers the BURNOUT → APOGEE state
transition.
Three tunable noise parameters (altitude process noise Q_alt, velocity
process noise Q_vel, and measurement noise R) are exposed as
milliscale Kconfig options so they can be adjusted without recompiling
the filter source.

A Python simulation tool
([`tools/sim_flight_kalman.py`](https://github.com/AUXSPACEeV/aurora/blob/main/tools/sim_flight_kalman.py))
reproduces the same algorithm with a realistic flight profile and MS5607
sensor-noise model, allowing the filter to be tuned and validated offline before
flight. On run, it produces a graph and saves it to a file called
`flight_simulation.png`. Here is an example run:

```bash
Plot saved to flight_simulation.png
Ground ref pressure: 101340 Pa (1013.4 hPa)
True apogee:         500.0 m
Filter apogee:       496.4 m (t = 12.56 s)
```

```{image} img/flight_simulation.png
:class: only-light
```

```{image} img/flight_simulation_dark.png
:alt: flight_simulation.png
:class: only-dark
```

The filter and its hypsometric pipeline are covered by a ztest suite under
([`aurora/tests/lib/apogee/`](https://github.com/AUXSPACEeV/aurora/tree/main/tests/lib/apogee)).
