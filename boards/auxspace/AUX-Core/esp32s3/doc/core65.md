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
AUX-Core is still under active hardware bring-up. The devicetree describes
rev 01 of the board as built, and {doc}`sensor_board
</applications/sensor_board>` builds and declares every chosen node it
needs, but none of it has been verified against real hardware yet.
```

# Hardware

- Two identical AUX-Stack data connectors (top and bottom), for stacking
  further brain boards and shields (e.g.
  {doc}`AUX-Pwr </boards/shields/AUX-Pwr/doc/pwr65>`) above and below the
  board. See [Connections and IOs](#connections-and-ios) below.
- BOOT button, a green STATE LED and an SD-activity LED. A third LED is
  wired straight to +3V3 as a power indicator and is not under software
  control.
- PWM buzzer and notifier LED (disabled by default). The buzzer
  sounds on assertion; the notifier LED is the STATE LED on LEDC channel 1.
- Sensors on the on-board I2C bus:
  - ST LSM6DSO32 6-DoF IMU, address `0x6a`, INT1 wired
  - ST LSM6DSV320X 6-DoF IMU with a 320 g high-g accelerometer, address
    `0x6b`, INT1 wired
  - TE MS5607-02BA barometer, address `0x77`, polled (no interrupt pin)
  - ST LIS3MDL magnetometer, address `0x1c`, DRDY wired
- Native (4-bit) SDHC µSD-card slot. `CD/DAT3` is shared, so the slot has
  no separate card-detect line.
- CAN/TWAI controller, shared between both AUX-Stack data connectors, behind
  a TI TCAN334 transceiver whose `STB` and `SHDN` pins are driven from GPIO
  (modelled as the `can_phy0` `can-transceiver-gpio` node).
- ESP32-S3 built-in Wi-Fi and Bluetooth LE
- USB-C with a CH224A PD sink (9V PD); USB Serial/JTAG for console, programming
  and debugging

There is no remove-before-flight connector on rev 01. The stack-wide `ARM`
signal takes its place as the arm interlock - see [Arming](#arming).

The board has no I2S or SPI peripherals: the pins those would use are taken
by the SD slot and the bottom connector's UART.

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

Both connectors are wired in parallel for the stack-wide "information"
signals, so the two `gpio-map`s point at the same three MCU pins. The signal
names are defined in
`<auxspace/dt-bindings/gpio/auxspaceev-aux-stack-data-connector.h>`:

| Signal             | Purpose                       | GPIO | Dir | Asserted |
|--------------------|-------------------------------|------|-----|----------|
| `AUX_DATA_ARM`     | Stack-wide arm/disarm state   | 15   | in  | high     |
| `AUX_DATA_LIFTOFF` | Stack-wide liftoff indication | 46   | out | high     |
| `AUX_DATA_PWRFLT`  | Stack-wide power-fault signal | 39   | out | **low**  |

`AUX_DATA_BAT_ON` is carried between the two connectors but is not wired to
the MCU on this board, so it is absent from both `gpio-map`s.

`LIFTOFF` and `PWRFLT` are outputs, grouped as `status_signals`. `ARM` is
treated as an **input** instead - see [Arming](#arming) - so it is not in
that group, since a pin cannot be both a `gpio-leds` output and an input.
Note that the Baseboard schematic sheet annotates `ARM` as an output of this
board; the devicetree deliberately differs.

### Arming

Rev 01 has no remove-before-flight connector, so the stack-wide `ARM` signal
is the arm authority: the board is armed while `ARM` is asserted. The
`arm_interlock` node (`auxspaceev,arm-interlock`) names that line, and
applications point the `auxspace,rbf` chosen node at it.

Because this is the opposite sense to a plug-style interlock - which is
asserted while it is *installed* and therefore safe - such an application
must also set `CONFIG_AURORA_STATE_MACHINE_RBF_ASSERT_ARMS=y`. Without it
the state machine reads an asserted `ARM` as SAFE.

Nothing on the board pulls `ARM`; the node requests an internal pull-down so
that an undriven stack reads SAFE rather than floating. The state machine
additionally always boots SAFE and arms only on an observed edge or an
explicit resync after a watchdog reset, so a stack already asserting `ARM`
at power-on does not come up armed.

The top and bottom connectors' UART and I2C pins are bridged by the 0 Ω
links R1/R3 (UART) and R5/R7 (I2C). The devicetree describes the I2C bridge
as fitted - both connectors name `aux_data_i2c` - and the UART bridge as
removed, giving each connector its own controller. Fitting R1/R3 shorts
`uart1` to `uart2`, so only one of the two may be enabled in that build.

Shields attach to a connector's I2C/UART pins (e.g.
{doc}`AUX-Pwr </boards/shields/AUX-Pwr/doc/pwr65>` attaches to
`aux_data_i2c`). CAN is a peer bus shared directly between brain boards and
does not go through the shield mechanism.

# Programming and Debugging

```{zephyr:board-supported-runners}
```

## Building

AUX-Core is built with `sysbuild` since it uses the MCUBoot boot loader.

```bash
west build -p -b core65/esp32s3/procpu --sysbuild sensor_board
```

## Flashing

The simplest way to flash is connecting the board via USB
and running:

```bash
west flash
```

Otherwise, put the board into download mode and flash both images
with `esptool`, then reset.
e.g. for the sensor_board application:

```bash
esptool --chip esp32s3 -p /dev/tty<ESP_DEV> -b 921600 write-flash \
  0x0 build/mcuboot/zephyr/zephyr.bin \
  0x20000 build/sensor_board/zephyr/zephyr.signed.bin
```
