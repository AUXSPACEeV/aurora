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

# Programming and Debugging

```{zephyr:board-supported-runners}
```
