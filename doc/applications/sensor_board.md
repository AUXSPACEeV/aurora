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

#### Timers and Timeouts

| Option | Unit | Default | Description |
|---|---|---|---|
| `CONFIG_BOOST_TIMER_MS` | ms | 900 | Duration that boost thresholds must be held before transitioning. |
| `CONFIG_LANDING_TIMER_MS` | ms | 500 | Duration that landing velocity must be held. |
| `CONFIG_APOGEE_TIMEOUT_MS` | ms | 60000 | Max time in APOGEE state before aborting to ERROR. |
| `CONFIG_MAIN_TIMEOUT_MS` | ms | 2000 | Delay between MAIN and REDUNDANT pyro events. |
| `CONFIG_REDUNDANT_TIMEOUT_MS` | ms | 900000 | Max time in REDUNDANT state before aborting. |



## Supported Boards and Shields

Since `sensor_board` is an auxspace internal project, only auxspace hardware
is tested with the application.

- {doc}`Sensor Board v2 <../boards/auxspace/sensor_board_v2/doc/sensor_board_v2>` - RP2040 / RP2350 flight computer
- {doc}`ESP32-S3 Micrometer <../boards/auxspace/esp32s3_micrometer/doc/esp32s3_micrometer>` - ESP32-S3 based board
