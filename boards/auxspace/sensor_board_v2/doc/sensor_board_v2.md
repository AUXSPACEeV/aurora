```{zephyr:board} sensor_board_v2
```

# Overview

The Auxspace e.V. sensor_board_v2 PCB is built around the idea to support
interchangeable hardware in a stackable rocket avionics system.
This version contains a
[Raspberry Pi Pico](https://www.raspberrypi.com/products/raspberry-pi-pico/)
with the RP2040 dual core Arm Cortex-M0+ or a
[Raspberry Pi Pico 2](https://www.raspberrypi.com/products/raspberry-pi-pico-2/)
with the RP2350 dual core RISC-V Hazard3.

# Hardware

## Raspberry Pi Pico variant:

- Dual core Arm Cortex-M0+ processor running up to 133MHz
- 264KB on-chip SRAM
- 2MB on-board QSPI flash with XIP capabilities
- 26 GPIO pins
- 3 Analog inputs
- 2 UART peripherals
- 2 SPI controllers
- 2 I2C controllers
- 16 PWM channels
- USB 1.1 controller (host/device)
- 8 Programmable I/O (PIO) for custom peripherals
- On-board LED
- 1 Watchdog timer peripheral
- Infineon CYW43439 2.4 GHz Wi-Fi chip (Pico W only)

## Raspberry Pi Pico 2 variant:

- Dual Cortex-M33 or Hazard3 processors at up to 150MHz
- 520KB of SRAM, and 4MB of on-board flash memory
- USB 1.1 with device and host support
- Low-power sleep and dormant modes
- Drag-and-drop programming using mass storage over USB
- 26 multi-function GPIO pins including 3 that can be used for ADC
- 2 SPI, 2 I2C, 2 UART, 3 12-bit 500ksps Analogue to Digital - Converter (ADC),
24 controllable PWM channels
- 2 Timer with 4 alarms, 1 AON Timer
- Temperature sensor
- 3 Programmable IO (PIO) blocks, 12 state machines total for custom peripheral
support
- Infineon CYW43439 2.4 GHz Wi-Fi chip (Pico 2W only)

  - Flexible, user-programmable high-speed IO
  - Can emulate interfaces such as SD Card and VGA

## Supported Features

```{zephyr:board-supported-hw}
```

## Connections and IOs

The connectors remain undocumented for the time being, since the PCB is under
active development.

### Wireless connectivity (`_w` variants)

The `_w` board variants are fitted with the Infineon CYW43439 combo radio (as
on the Raspberry Pi Pico W / Pico 2 W). It hangs off a PIO-SPI ("gSPI") bus on
GPIO 23/24/25/29, defined in `sensor_board_v2_wifi.dtsi`.

### Wi-Fi (supported)

Wi-Fi works through the upstream `infineon,airoc-wifi` driver. Enable it with
`CONFIG_WIFI=y` / `CONFIG_WIFI_AIROC=y` plus the module selection
(`CONFIG_CYW43439=y`, `CONFIG_CYW43439_MURATA_1YN=y`).

### Bluetooth / BLE (**NOT** supported)

```{warning}
There is currently **no way to enable Bluetooth on the `_w` boards through
configuration alone.**
The CYW43439's Bluetooth shares the same PIO-SPI bus as Wi-Fi, and
**Zephyr has no HCI transport driver for CYW43439 BT over that shared bus**
(verified against the *Zephyr 4.4* tree).
```

Why config-only attempts fail:

- The only AIROC Bluetooth HCI driver in Zephyr is `hci_uart_infineon.c`
  (`compatible = "infineon,bt-hci-uart"`), which requires a **dedicated BT
  UART** (see the `cy8cproto_062_4343w` board). Every CYW43439 BT Kconfig
  option (`CONFIG_CYW43439`, ...) gates on `BT_H4` (UART). The Pico W does not
  route BT out on a UART; its BT is multiplexed on the Wi-Fi PIO SPI bus.
- The Zephyr 4.4 migration guide folded the old combo-chip compatible into the
  UART driver: `infineon,cyw43xxx-bt-hci` => `infineon,bt-hci-uart`,
  `CONFIG_BT_CYW43XX` => `CONFIG_BT_HCI_UART_INFINEON`.
- Upstream `rpi_pico_rp2040_w.dts` deliberately wires Wi-Fi only — no BT node,
  no `chosen { zephyr,bt-hci }`.

Symptom of trying anyway: declaring a `bt-hci` / `infineon,airoc-bt-hci` node
plus `chosen { zephyr,bt-hci = ... }` makes the BT host `DEVICE_DT_GET()` the
node, but no driver has a matching `DT_DRV_COMPAT`, so the link fails with
`undefined reference to __device_dts_ord_<N>` (the node number shifts as the DT
is restructured, but the cause is always "no driver behind the compatible").

The only code that drives CYW43439 BT over the shared bus is the Pico SDK's
BTStack transport (`hal_rpi_pico/.../pico_cyw43_driver`), which is a separate
host stack and is **not** wired into Zephyr's Bluetooth subsystem.

Enabling BLE here is therefore a driver-development task (port the BTStack
cyw43 transport to a Zephyr `bt-hci` driver with Wi-Fi/BT bus arbitration), not
a configuration task.

# Programming and Debugging

```{zephyr:board-supported-runners}
```

```{warning}
If no µSD-Card is inserted, the board will have trouble booting, since the
SPI-SDHC driver has no way to detect card presence without a card-detect-pin!
```

## Building

### RP2040

The aurora software for the RP2040 is built with `sysbuild` since it uses the MCUBoot boot loader:

```bash
west build -p -b sensor_board_v2/rp2040 --sysbuild sensor_board
```

### RP2350
The aurora software for the RP2350 is built without `sysbuild` at the moment.

```bash
west build -p -b sensor_board_v2/rp2350a/hazard3 sensor_board
```

## Flashing

## RP2040

Flashing the MCUBoot bootloader can not be done with `west flash` since it does not
install both binary files in the correct location in the flash. Instead the software
can be flashed with `picotool` allowing flashing via the USB interface. For this
the board needs to be put into bootloader mode by holding the `BOOTSEL` button while
plugging the board into the computer. Then the following commands can be used to flash the bootloader and the application:

```bash
# Flash the bootloader
picotool load build/mcuboot/zephyr/zephyr.uf2
# Flash the application
picotool load build/sensor_board/zephyr/zephyr.signed.bin --offset 0x10010000
# Reboot the board
picotool reboot
```

````{note}
```picotool```  can be obtained either by building it from source following the
instructions on the [picotool](https://github.com/raspberrypi/picotool) repository or by installing it as a prebuilt binary from the release page of the [pico-sdk-tools](https://github.com/raspberrypi/pico-sdk-tools/releases) repository.
````

## RP2350

Flashing the RP2350 can be done by copying the generated `zephyr.uf2` located in the
`build/sensor_board/zephyr` directory to the board's USB mass storage device. The board needs to be put into bootloader mode by holding the `BOOTSEL` button while plugging the board into the computer.
