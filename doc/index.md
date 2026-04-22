# AURORA

**AURORA** (AUxspace ROcket opeRAting System) is a Zephyr RTOS-based avionics
firmware for rockets, developed by [Auxspace e.V.](https://auxspace.de).

It manages sensor data, flight state detection, and pyrotechnic ignition across
multiple interconnected boards communicating over CAN bus.

**AURORA** itself is made up of several key components:

- applications
- boards
- drivers
- libraries
- tests
- tools

As depicted in the
[Application Development](https://docs.zephyrproject.org/latest/develop/application/)
guide from the
[Zephyr Project Documentation](https://docs.zephyrproject.org/latest/index.html),
AURORA acts as a
[Zephyr freestanding application](https://docs.zephyrproject.org/latest/develop/application/#zephyr-freestanding-application).

## AURORA Libraries and Drivers

The core parts of AURORA are its libraries and drivers, that can be used by any
Zephyr Application with simple `#include <aurora/...>` statements,
[`devicetree overlays`](https://docs.zephyrproject.org/latest/build/dts/index.html)
and
[`Kconfigs`](https://docs.zephyrproject.org/latest/build/kconfig/index.html).

Libraries are located at
[`lib`](https://github.com/AUXSPACEeV/aurora/tree/main/lib) while drivers can be
found in the [`drivers`](https://github.com/AUXSPACEeV/aurora/tree/main/drivers)
directory.

Their public APIs are documented in the API-Reference, including the
[library](lib/index.md) and the [driver](drivers/index.md) APIs.

## AURORA Applications

Applications use libraries and drivers to function.
An example is the
[`sensor_board`](https://github.com/AUXSPACEeV/aurora/tree/main/sensor_board)
application, that acts as a flight computer using IMU and barometric pressure
data and deploying a parachute when rocket apogee is detected.

Since application's use cases vary, `Kconfigs` and `Devicetree Overlays` are
used to customize the application's function for each board.
Some boards may have additional pyro ignitions or the capability to signal
state machine changes on a buzzer while others perform communication tasks
over CANbus and use redundant sensor data (e.g. High G Accelerometer combined
with the standard 6 or 9-DoF IMU).

## Project

```{toctree}
:maxdepth: 2
:caption: User Guide

getting_started
configuration
testing
```

```{toctree}
:maxdepth: 2
:caption: Design

applications/index
zephyr
```

```{toctree}
:maxdepth: 2
:caption: Reference

drivers/index
lib/index
tools/index
```

```{toctree}
:maxdepth: 1
:caption: Boards

boards/index
```

## Indices and tables

- {ref}`genindex`
- {ref}`search`
