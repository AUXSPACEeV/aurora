# Sensor Board

The sensor board application is a simple flight computer for model rocketry.

AURORA is built to run on multiple interconnected PCBs that communicate over CAN
bus, i2c or other fieldbus technologies.
`sensor_board` manages the IMU and barometric sensor data, uses the flight state
machine, pyrotechnic ignition, hardware notification and data logging and is
designed to deploy a parachute when the model rocket reaches apogee.

## Sensor Data Path

It's a tricky situation trying to fetch data from all sensors at the exact same
time and passing them on to the state machine, the state machine filter and the
data logger.
Here is a rough overview of how AURORA's data is passed between threads to the
state machine and the data logger:

![sensors datapath](/img/sensor_datapath.drawio.svg)

## Threads

`sensor_board` uses up to three additional Zephyr threads
(all priority 5, 2 KB stack):

| Thread | Guard | Purpose |
|---|---|---|
| `imu_task` | `CONFIG_IMU` | Polls IMU at the configured frequency, updates orientation and acceleration globals. |
| `baro_task` | `CONFIG_BARO` | Measures pressure/temperature, computes altitude. |
| `state_machine_task` | `CONFIG_AURORA_STATE_MACHINE` | Runs at 10 Hz. Feeds sensor data into the state machine and fires pyro channels on state transitions. |

When sensors are configured to use active polling, `baro_task` and `imu_task`
run the whole lifetime of the application.
When baro, imu or both are using interrupt triggers, `baro_task` and `imu_task`
complete after initialisation and the generic sensor trigger threads are
running.

```{note}
To get an overview of running threads, use the Zephyr-Shell builtin command
`kernel threads list`.
```

## Configuration

### State Machine Thresholds

These options are defined in `sensor_board/Kconfig` under the
**State Machine Configuration** menu.

#### Flight Thresholds

| Option | Unit | Default | Description |
|---|---|---|---|
| `CONFIG_BOOST_ACCELERATION` | m/s² | 30 | Acceleration threshold for boost detection (ARMED -> BOOST). |
| `CONFIG_BOOST_ALTITUDE` | m | 50 | Altitude threshold for boost detection (ARMED -> BOOST). |
| `CONFIG_BURNOUT_ACCELERATION` | m/s² | 15 | Acceleration threshold for burnout detection (BOOST -> BURNOUT). |
| `CONFIG_MAIN_DESCENT_HEIGHT` | m | 200 | Descent altitude for main pyro event (APOGEE -> MAIN). |
| `CONFIG_LANDING_VELOCITY` | m/s | 2 | Velocity threshold for landing detection. |
| `CONFIG_ARM_ANGLE` | deg | 85 | Orientation threshold for arming (IDLE -> ARMED). 0 = horizontal, 90 = vertical. |
| `CONFIG_DISARM_ANGLE` | deg | 70 | Orientation threshold for disarming (ARMED -> IDLE). |
| `DISARM_ANGLE_SAMPLES` | n | 10 | Number of consecutive samples for disarming (ARMED -> IDLE). |

#### Timers and Timeouts

| Option | Unit | Default | Description |
|---|---|---|---|
| `CONFIG_BOOST_TIMER_MS` | ms | 900 | Duration that boost thresholds must be held before transitioning. |
| `CONFIG_LANDING_TIMER_MS` | ms | 500 | Duration that landing velocity must be held. |
| `CONFIG_APOGEE_TIMEOUT_MS` | ms | 60000 | Max time in APOGEE state before aborting to ERROR. |
| `CONFIG_MAIN_TIMEOUT_MS` | ms | 2000 | Delay between MAIN and REDUNDANT pyro events. |
| `CONFIG_REDUNDANT_TIMEOUT_MS` | ms | 900000 | Max time in REDUNDANT state before aborting. |

## Application Simulation

When built with `CONFIG_AURORA_FAKE_SENSORS=y` (typically together with the
`native_sim` board target), `sensor_board` replaces the real IMU and baro
polling threads with a synthetic data source. The fake threads publish on
the same zbus channels (`imu_data_chan`, `baro_data_chan`) at the same
cadence as the real drivers, so the state machine, filter, data logger and
pyro logic run unchanged.

Two backends sit behind the same shell interface:

| Backend | Kconfig | Source of samples |
|---|---|---|
| Synthetic profile | `AURORA_FAKE_SENSORS_SYNTH` (default) | Analytic ISA-troposphere flight profile generated on the fly. |
| Replay | `AURORA_FAKE_SENSORS_REPLAY` | Samples from a recorded `flights.csv`, embedded in the firmware at build time. |

### Synthetic profile

The default profile follows an ISA-troposphere altitude/pressure curve with
a constant-thrust boost phase, a ballistic coast to apogee and a
constant-rate parachute descent. It is controlled from the Zephyr shell via
the `sim` command group:

| Command | Description |
|---|---|
| `sim launch` | Start the synthetic flight profile. The uptime at which this command is issued is used as t=0. |
| `sim reset` | Return the profile to pad-stationary (altitude 0, accel = +g on the vertical axis). |
| `sim status` | Print the current flight time, altitude (m) and vertical proper-acceleration (m/s²), or `pad-stationary` if no launch is active. |

Using the `native_sim` board target, and the
[`sim_fetch.py`](/tools/sim_fetch.md) tool, it is possible to run application
simulations and create graphs from the data_logger output:

```{image} /flight_logs/sim_fetch/plots/flight1_light.png
:alt: flight1_light.png
:class: only-light
```

```{image} /flight_logs/sim_fetch/plots/flight1_dark.png
:alt: flight1_dark.png
:class: only-dark
```

### Replay backend

Setting `CONFIG_AURORA_FAKE_SENSORS_REPLAY=y` swaps the analytic profile
for a playback engine that streams a previously recorded flight back into
the system at the original cadence. The samples are baked into the
firmware image at build time, so no filesystem or network access is needed
at runtime — useful for CI runs and for regression-testing the Kalman
filter / state machine against known-good data.

The recording is selected via:

```kconfig
CONFIG_AURORA_FAKE_SENSORS_REPLAY_INPUT="flight_logs/<campaign>/<flight>/flights.csv"
```

(Paths are relative to the aurora module root.) During the build, the
[`gen_flight_replay.py`](/tools/gen_flight_replay.md) tool turns the CSV —
and, if a sibling `state_audit` file is present, the trimmed
`[BOOST - 4 s, LANDED + 4 s]` window — into a generated `replay_data.c`
that is linked into the firmware.

Before launch, the replay threads keep the rocket "pad-stationary" by
republishing the very first recorded sample, so attitude calibration
converges as it would on the real hardware. Issuing `sim launch` from the
shell (or letting `CONFIG_AURORA_SIM_AUTOTEST=y` do it automatically) then
starts the playback.

## Supported Boards and Shields

Since `sensor_board` is an auxspace internal project, only auxspace hardware
is tested with the application.

- {doc}`Sensor Board v2 <../boards/auxspace/sensor_board_v2/doc/sensor_board_v2>` - RP2040 / RP2350 flight computer
- {doc}`ESP32-S3 Micrometer <../boards/auxspace/esp32s3_micrometer/doc/esp32s3_micrometer>` - ESP32-S3 based board

```{important}
Flight computers often don't have on-board storage, so a µSD-Card (at least 16 GiB) is needed additionaly.
```
