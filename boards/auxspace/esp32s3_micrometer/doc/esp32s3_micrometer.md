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

```{warning}
If no µSD-Card is inserted, the board will have trouble booting, since the
SPI-SDHC driver has no way to detect card presence without a card-detect-pin!
```

## Building

µMeter is built with `sysbuild` since it uses the MCUBoot boot loader:

```bash
west build -p -b esp32s3_micrometer/esp32s3/procpu --sysbuild sensor_board
```

## Flashing

`west flash` is unfortunately not working with this setup, since the ESP
requires the download mode to be set when booting and west flash is chain
loading both images after one another.

Instead, flash both images in download mode and then just reset, using
`esptool`:

```bash
esptool --chip esp32s3 -p /dev/tty<ESP_DEV> -b 921600 write-flash \
  0x0 build/mcuboot/zephyr/zephyr.bin \
  0x20000 build/sensor_board/zephyr/zephyr.signed.bin
```
