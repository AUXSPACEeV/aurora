# AUX-Pwr 65

## Overview

The Auxspace e.V. AUX-Pwr 65 PCB is the power management board of the
**AUX-Stack**, a modular avionics stack made up of interconnected PCBs. It
has no MCU of its own: it plugs onto a "brain board" (e.g.
{doc}`AUX-Core </boards/auxspace/AUX-Core/esp32s3/doc/core65>`) via an
AUX-Stack data connector and is driven entirely over I2C, as a Zephyr
[shield](https://docs.zephyrproject.org/latest/hardware/porting/shields.html).

```{warning}
AUX-Pwr is still under active hardware bring-up. Both of its devicetree
nodes are `status = "disabled"` by default until the corresponding
drivers/wiring have been validated.
```

## Hardware

- TI TCA9534 8-bit I2C GPIO expander
- TI INA219 current/power monitor

## Connections and IOs

AUX-Pwr attaches to the `aux_data_i2c` bus alias exposed by its brain
board's AUX-Stack data connector. It therefore only works on boards that
define that alias — currently {doc}`AUX-Core
</boards/auxspace/AUX-Core/esp32s3/doc/core65>` (`core65`).

| Device | I2C address | Node |
|---|---|---|
| TCA9534 GPIO expander | `0x20` | `tca9534_0` |
| INA219 current/power monitor | `0x81` | `ina219_0` |

## Usage

As a shield, AUX-Pwr is not a `west build -b` target on its own. Instead it
is layered on top of a brain board build with `--shield pwr65`, e.g. for the `core65` board:

```bash
west build -p -b core65/esp32s3/procpu --shield pwr65 --sysbuild sensor_board
```

The shield only adds the two devicetree nodes above; enable the ones you
need for your application (e.g. via a board/application overlay) since both
default to `status = "disabled"`.
