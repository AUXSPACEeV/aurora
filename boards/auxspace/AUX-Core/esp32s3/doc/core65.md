```{zephyr:board} core65
```

# Overview

The Auxspace e.V. AUX-Core 65 PCB is the "brain board" at the heart of the
**AUX-Stack**, a modular avionics stack made up of interconnected PCBs.
It is built around an
[ESP32-S3-WROOM-1](https://www.espressif.com/en/products/modules/esp32-s3)
module (8 MB flash, 8 MB PSRAM) from
[Espressif](https://www.espressif.com/).

More about the SoC itself can be found in the zephyr docs of the
[ESP32-S3](https://docs.zephyrproject.org/latest/boards/espressif/esp32s3_devkitc/doc/index.html).

```{warning}
AUX-Core is still under active hardware bring-up. Most of its sensor
footprints are wired in devicetree but left `status = "disabled"` pending
validation, and no AURORA application currently declares the chosen nodes
it would need to run on this board (see the
{doc}`sensor_board hardware requirements </applications/sensor_board>`).
```

# Hardware

- Two identical AUX-Stack data connectors (top and bottom), for stacking
  further brain boards and shields (e.g.
  {doc}`AUX-Pwr </boards/shields/AUX-Pwr/doc/pwr65>`) above and below the
  board. See [Connections and IOs](#connections-and-ios) below.
- BOOT button and an SD-activity status LED
- PWM buzzer and notifier LED footprints (disabled by default)
- Sensor footprints on the on-board I2C bus, all disabled by default
  pending bring-up:
  - ST LSM6DSO32 6-DoF IMU
  - Infineon DPS310 barometer
  - Analog Devices ADXL367 accelerometer
  - Melexis MLX90395 magnetometer
- Native (4-bit) SDHC µSD-card slot
- CAN/TWAI controller, shared between both AUX-Stack data connectors
- ESP32-S3 built-in Wi-Fi and Bluetooth LE
- USB Serial/JTAG for console, programming and debugging
- Capacitive touch input controller

## Supported Features

```{zephyr:board-supported-hw}
```

## Connections and IOs

A detailed physical pinout diagram is not yet available, since the PCB is
under active development.

### AUX-Stack data connectors

AUX-Core exposes two `auxspaceev,aux-stack-data-connector` connectors,
`top_data_connector` and `bottom_data_connector`, for stacking further
boards above and below it. Both connectors share the same CAN bus and the
same I2C bus (`aux_data_i2c`), but each has its own dedicated UART
(`aux_data_uart_top` / `uart1` for the top connector,
`aux_data_uart_bottom` / `uart2` for the bottom one).

Each connector also breaks out three stack-wide "information" signals as
GPIOs, defined in
`<auxspace/dt-bindings/gpio/auxspaceev-aux-stack-data-connector.h>`:

| Signal        | Purpose                          |
|---------------|-----------------------------------|
| `AUX_DATA_ARM`     | Stack-wide arm/disarm state   |
| `AUX_DATA_LIFTOFF` | Stack-wide liftoff indication  |
| `AUX_DATA_PWRFLT`  | Stack-wide power-fault signal |

Shields attach to a connector's I2C/UART pins (e.g.
{doc}`AUX-Pwr </boards/shields/AUX-Pwr/doc/pwr65>` attaches to
`aux_data_i2c`). CAN is a peer bus shared directly between brain boards and
does not go through the shield mechanism.

# Programming and Debugging

```{zephyr:board-supported-runners}
```

## Building

AUX-Core is built with `sysbuild` since it uses the MCUBoot boot loader.
Since no AURORA application is wired up for this board yet, use a plain
Zephyr sample to validate the board itself:

```bash
west build -p -b core65/esp32s3/procpu --sysbuild sensor_board
```

## Flashing

`west flash` does not work here, since the ESP32-S3 requires download mode
to be set when booting and `west flash` chain-loads both images one after
another.

Instead, put the board into download mode and flash both images with
`esptool`, then reset:

```bash
esptool --chip esp32s3 -p /dev/tty<ESP_DEV> -b 921600 write-flash \
  0x0 build/mcuboot/zephyr/zephyr.bin \
  0x20000 build/hello_world/zephyr/zephyr.signed.bin
```
