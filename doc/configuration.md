# Configuration

All AURORA features are toggled and tuned through Kconfig.  Use
`west build -t menuconfig` or `./run.sh -b <board> menuconfig` to
browse options interactively.

By hitting the `D`, menuconfig supports minimal config saving.
After saving a minimal config, run `tools/sync_defconfig.py` to merge
the minimal config automatically into the desired project.

````{warning}
sync_defconfig.py is not an official part of zephyr, so use the script at own
risk! (The high risk being that a config might be different from what you were
expecting)
````

## Feature Toggles

| Option | Default | Description |
|---|---|---|
| `CONFIG_AURORA_SENSORS` | `n` | Enable the sensor hardware abstraction layer. |
| `CONFIG_IMU` | `n` | Enable the inertial measurement unit (requires `AURORA_SENSORS`). |
| `CONFIG_BARO` | `n` | Enable the barometric pressure sensor (requires `AURORA_SENSORS`). |
| `CONFIG_AURORA_STATE_MACHINE` | `n` | Enable the flight state machine library. |
| `CONFIG_SIMPLE_STATE` | `y` | Select the simple 9-state flight state machine (default when state machine is enabled). |
| `CONFIG_APOGEE_DETECTION` | `n` | Enable Kalman-filter-based apogee detection. |
| `CONFIG_PYRO` | `n` | Enable pyrotechnic ignition drivers. |
| `CONFIG_SERVO` | `n` | Enable servo control. |

## Sensor Configuration

### IMU

| Option | Default | Description |
|---|---|---|
| `CONFIG_IMU_FREQUENCY_VALUE` | `10` | Polling rate in Hz. Choices: 10, 50, 100, 200, 400. |

### Barometer

| Option | Default | Description |
|---|---|---|
| `CONFIG_BARO_FREQUENCY_VALUE` | `10` | Polling rate in Hz. Choices: 10, 50, 100, 200, 400. |
| `CONFIG_BARO_OVERSAMPLING_VALUE` | `256` | ADC oversampling rate. Choices: 256, 512, 1024, 2048, 4096. |

## State Machine Thresholds

These options are defined in `sensor_board/Kconfig` under the
**State Machine Configuration** menu.

| Option | Unit | Default | Description |
|---|---|---|---|
| `CONFIG_BOOST_ACCELERATION` | m/s² | 30 | Acceleration threshold for boost detection (ARMED -> BOOST). |
| `CONFIG_BOOST_ALTITUDE` | m | 50 | Altitude threshold for boost detection (ARMED -> BOOST). |
| `CONFIG_BURNOUT_ACCELERATION` | m/s² | 15 | Acceleration threshold for burnout detection (BOOST -> BURNOUT). |
| `CONFIG_MAIN_DESCENT_HEIGHT` | m | 200 | Descent altitude for main pyro event (APOGEE -> MAIN). |
| `CONFIG_LANDING_VELOCITY` | m/s | 2 | Velocity threshold for landing detection. |
| `CONFIG_ARM_ANGLE` | deg | 85 | Orientation threshold for arming (IDLE -> ARMED). 0 = horizontal, 90 = vertical. |
| `CONFIG_DISARM_ANGLE` | deg | 70 | Orientation threshold for disarming (ARMED -> IDLE). |

### Timers and Timeouts

| Option | Unit | Default | Description |
|---|---|---|---|
| `CONFIG_BOOST_TIMER_MS` | ms | 900 | Duration that boost thresholds must be held before transitioning. |
| `CONFIG_LANDING_TIMER_MS` | ms | 500 | Duration that landing velocity must be held. |
| `CONFIG_APOGEE_TIMEOUT_MS` | ms | 60000 | Max time in APOGEE state before aborting to ERROR. |
| `CONFIG_MAIN_TIMEOUT_MS` | ms | 2000 | Delay between MAIN and REDUNDAND pyro events. |
| `CONFIG_REDUNDAND_TIMEOUT_MS` | ms | 900000 | Max time in REDUNDAND state before aborting. |

## Apogee Detection (Kalman Filter)

Noise parameters are integer-scaled by 1000 in Kconfig.
The actual float value is `CONFIG_FILTER_*_MILLISCALE / 1000.0`.

| Option | Default | Description |
|---|---|---|
| `CONFIG_FILTER_Q_ALT_MILLISCALE` | 100 | Process noise for altitude state (Q_alt). Increase if altitude estimate lags behind reality. |
| `CONFIG_FILTER_Q_VEL_MILLISCALE` | 500 | Process noise for velocity state (Q_vel). Increase for more aggressive response during boost. |
| `CONFIG_FILTER_R_MILLISCALE` | 4000 | Measurement noise variance (R). Higher values trust the barometer less. |

## Pyro Drivers

| Option | Default | Description |
|---|---|---|
| `CONFIG_PYRO_INIT_PRIORITY` | 80 | Kernel init priority for pyro driver instances. |

