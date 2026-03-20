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
`CONFIG_STATE_MACHINE_TYPE`.
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

## Supported Boards

| Board | MCU | Notes |
|---|---|---|
| `sensor_board_v2/rp2040` | RP2040 (Cortex-M0+) | Primary target |
| `sensor_board_v2/rp2350a/hazard3` | RP2350 (RISC-V Hazard3) | |
| `esp32s3_micrometer/esp32s3/procpu` | ESP32-S3 | Custom Auxspace board |
