```{zephyr:board} esp32s3_micrometer
```

# Overview

The Auxspace e.V. ESP32-S3-µMETER PCB is a minimal flight computer on a 4-layer
PCB with an ESP32-S3-WROOM-1 from [Espressif](https://www.espressif.com/).

# Hardware

ESP32-S3 is a low-power MCU-based system on a chip (SoC) with integrated 2.4 GHz
Wi-Fi and Bluetooth® Low Energy (Bluetooth LE). It consists of high-performance
dual-core microprocessor (Xtensa® 32-bit LX7), a low power coprocessor, a Wi-Fi
baseband, a Bluetooth LE baseband, RF module, and numerous peripherals.
More about that can be found in the zephyr docs of the
[ESP32-S3](https://docs.zephyrproject.org/latest/boards/espressif/esp32s3_devkitc/doc/index.html).

Specifically added features include:

- 6-DoF IMU
- Barometer
- Buzzer
- 2-channel Pyro ignition
- CAN-bus for communication with posible expansion PCBs
- µSD-Card slot

## Supported Features

```{zephyr:board-supported-hw}
```

## Connections and IOs

- UART for programming and debugging
- SW1 for inputs
- Two seperated pin-pairs for the pyros
- Pins for battery
- 6-pin connector for expansion PCBs;
don't throw heavy loads an the 3V3 pin, it may overheat the LDO.

# Programming and Debugging

```{zephyr:board-supported-runners}
```
