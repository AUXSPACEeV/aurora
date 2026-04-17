# Getting Started

## Prerequisites

In the [AURORA git repository's README](https://github.com/AUXSPACEeV/aurora)
and the {external+zephyr:ref}`getting_started` guide from `zephyr` is everything
you need to install first.

AURORA is built inside a `west` workspace.
The directory layout is:

```
zephyr_workspace/
├── aurora/          ← this repository
├── modules/
├── .west/
└── zephyr/
```

All `west` commands must be run from the `aurora/` directory.

## Building

Build the primary `sensor_board` application for one of the supported boards:

```shell
# RP2040 (primary target)
west build -b sensor_board_v2/rp2040 sensor_board

# RP2350 RISC-V
west build -b sensor_board_v2/rp2350a/hazard3 sensor_board

# ESP32-S3 Micrometer board
west build -b esp32s3_micrometer/esp32s3/procpu --sysbuild sensor_board/
```

````{note}
Build output is located at `build/zephyr/zephyr.uf2` and
`build/zephyr/zephyr.elf` for the default builds, while `--sysbuild` generates
subdirectories for `MCUBoot` and the application at
`build/mcuboot/zephyr/zephyr.elf` and
`build/sensor_board/zephyr/zephyr.signed.bin`.
````

### Interactive Kconfig

```shell
# Using west
west build -b \<BOARD\> -t menuconfig \<APPLICATION\>

# Using the wrapper
./run.sh -b sensor_board_v2/rp2040 menuconfig
```

### Docker Container

```shell
# Open a shell inside the dev container
./run.sh -b sensor_board_v2/rp2040 shell
```

## Flashing

### Zephyr's West

```shell
west flash
```

````{note}
Boards like the esp32s3 that use an interactive download mechanism need to
trigger the download mechanism **twice** for sysbuild binaries, since flashing
multiple binaries require multiple reboots.

In case of the esp32s3, start by booting in download mode, run the flashing
command and wait for the first binary to be flashed.

Reboot the board by cutting and re-establishing power to it, while still
asserting the switch that activates download mode.

Repeat until the flashing command has finished.
````

### OpenOCD (CMSIS-DAP)

```shell
# e.g. sensor_board_v2/rp2040
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
  -c "adapter speed 5000" \
  -c "program build/zephyr/zephyr.elf verify reset exit"
```

### USB (on sensor_board_v2 via RPi Pico BOOTSEL mode)

Hold BOOTSEL while connecting the board, then copy the UF2 file:

```shell
cp build/zephyr/zephyr.uf2 /media/${USER}/RPI-RP2
```
