# `AURORA`

> **AU**xspace **RO**cket ope**RA**ting System

## General Info

### :warning: Disclaimer

This is the project's info page.
For detailed documentation head to
[aurora.auxspace.de](https://aurora.auxspace.de/).

### Technical Terms (for Student Newcomers)

* `RTOS`: **R**eal-**T**ime **O**perating **S**ystem
* `µC`: **Mi**cro**C**ontroller (µ - Controller)
* `MCU`: **M**icro**C**ontroller **U**nit
* `SOC`: **S**ystem **O**n **C**hip
* `Zephyr`: Open-Source RTOS Kernel for a large variety of MCUs and SOCs
(<https://www.zephyrproject.org/>)
* `fork`: A *forked* repository is a direct clone of another, with custom
modifications made
* `Docker`: "Docker Engine is an open source containerization technology
for building and containerizing your applications"
(<https://docs.docker.com/engine/>)
* `devicetree`: Tree-like configuration for hardware integration
(<https://docs.kernel.org/devicetree/index.html>)

### Introduction

**AURORA** is an open-source software project developed by Auxspace e.V.,
a student-driven initiative dedicated to building rockets and providing
hands-on aerospace engineering experience.
This project is specifically designed for the **Auxspace e.V. Rocket
Electronics Stack**, incorporating a modular electronics stack for
optimal performance and flexibility.

The **AURORA** system leverages a series of interconnected PCBs
communicating over **CAN**, with each PCB housing a microchip running
either the **AURORA** software or adding value to it (e.g. GPIO port
expanders, pyro ignition channels or long-range communication modules).
The primary purpose of **AURORA** is to serve as avionics software,
managing essential data such as air pressure, acceleration, gyro, and
magnetometer readings in a plug-and-play state machine with configurable
filters and events.
Using the [zephyr](https://docs.zephyrproject.org/latest/) kernel,
**AURORA** handles task scheduling and real-time operations, enabling
reliable and efficient communication between multiple **AURORA**
instances through **CAN**.

Join us as we continue to develop and refine **AURORA**,
contributing to the advancement of rocket technology and student
expertise in aerospace engineering!

## :hammer_and_wrench: Setup

Setup zephyr using either the docker container in this directory or follow the
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
from the Zephyr project.

### Native

First, create a workspace:

```bash
mkdir zephyr_workspace && cd zephyr_workspace
```

Then, install the system dependencies:

x86 Debian Linux:

```bash
sudo apt install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget python3-dev python3-venv python3-tk \
  xz-utils file make gcc libsdl2-dev libmagic1

# x86 specific
sudo apt-get install -y --no-install-recommends \
    gcc-multilib g++-multilib
```

or if you are on fedora:

```bash
sudo dnf install -y \
    git cmake ninja-build gperf \
    ccache dfu-util dtc wget python3-devel python3-virtualenv python3-tkinter \
    xz file make gcc gcc-c++ glibc-devel.i686 libstdc++-devel.i686 \
    SDL2-devel file-libs
```

Using a python virtualenv, install west, zephyr's pythondependencies and
the zephyr sdk with the required toolchains:

```bash
python3 -m venv .venv
source .venv/bin/activate

python3 -m pip install west

west init -m https://github.com/AUXSPACEeV/aurora --mr main .
cd aurora
west update
west zephyr-export
west packages pip --install

west sdk install -t \
    riscv64-zephyr-elf \
    arm-zephyr-eabi \
    xtensa-espressif_esp32s3_zephyr-elf \
    x86_64-zephyr-elf
```

This should leave a directory setup like so:

```bash
zephyr_workspace
├── aurora
├── modules
├── .venv
├── .west
└── zephyr
```

### Docker

<details> <summary> <b>Step-by-step guide</b> (<i>click</i> to open) </summary>

**Build Dependencies**

If you don't want to install all Zephyr dependencies by yourself and system-wide,
this repository also comes with a configured docker container.

To install docker, head to the
[docker install instructions](https://docs.docker.com/engine/install/),
select your distro and follow the instructions.

[podman](https://podman.io/docs/installation)
also works for this project and can be selected using the `--engine`
option in the `run.sh` wrapper.

**Directory setup**

Create a workspace and add the aurora app to it:

```bash
mkdir zephyr_workspace
cd zephyr_workspace
git clone -b main https://github.com/AUXSPACEeV/aurora.git aurora
cd aurora
```

**Container and Zephyr Workspace**

After installing docker, all requirements are met to run the setup:

```bash
# Open a shell in the development container for the sensor_board_v2/rp2040 board
./run.sh -b sensor_board_v2/rp2040 shell
```

**note:** This can take a long time, since the docker container is quite big
and ghcr.io is rather slow.
Build the container yourself with the `--rebuild` option or just use `CTRL + C`
when the container is being pulled.

The Zephyr workspace is configured to be at *$(pwd)/..*.
Zephyr itself is then found at *zephyr_workspace/zephyr* and this
application at *zephyr_workspace/aurora*.

This should leave a directory setup like so:

```bash
zephyr_workspace
├── aurora
├── modules
├── .west
└── zephyr
```

*zephyr_workspace* is mounted into the container, so changes that you perform
inside the container will take effect outside and vice-versa.

Run `west update` in *zephyr_workspace/aurora* to update modules.

</details>

## :hammer: Build

The important directories in this project are

1. **\<zephyr_workspace\>**
2. **\<aurora\>**

If you ran
`west init -m https://github.com/AUXSPACEeV/aurora --mr main .`
in **~/zephyr_workspace** then **\<aurora\>** will be at
**~/zephyr_workspace/aurora**.

The following commands are examples of how to use west when building an
application.
Start the command from the **\<aurora\>** dir:

```bash
# Build the sensor_board project for the sensor_board_v2/rp2040
west build -b sensor_board_v2/rp2040 sensor_board

# Another example: build for the custom Auxspace Micrometer PCB:
west build -b esp32s3_micrometer/esp32s3/procpu sensor_board/
```

The output from the build will be at
**\<aurora\>/build/zephyr**,
called `zephyr.uf2` and `zephyr.elf`.

## :rocket: Deployment

**note:** This chapter shows how to deploy AURORA onto the
sensor_board_v2 as an example.
The board uses a Raspberry Pi Pico Series microcontroller as its main
logic unit.

### West

Deploy the binary to your board by either using `west flash` or the
Board manufacturers' manual.
In case of the sensor_board_v2 that would be
[Raspberry Pi](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html).

**note:** flashing from inside the docker container is currently
untested and unimplemented.

### Openocd

The following is an example on how to flash **AURORA** onto the Pi
using openocd:

```bash
# Flash the PI
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000" -c "program build/zephyr/zephyr.elf verify reset exit"

# Attach to debug probe
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000"

# Connect gdb to openocd
gdb build/zephyr/zephyr.elf
>target remote localhost:3333
>monitor reset init

# Start debugging!
```

### Standard USB Interface

The RP2040 and RP2350 come with a backup bootloader, in case the QSPI Flash
is not responding to the ROM bootloader.
The `BOOTSEL` switch deasserts the QSPI Chip Select pin and opens up the
fallback loader, which is a simple USB interface.
When connecting the PI to your PC in this state, the PI is registered as
an external drive called `RPI-RP2`, which can load `.uf2` files by simply
copying these firmware files onto the drive.

Copy the files from your drive to the Pi's filesystem like so, or with
your file explorer:

```bash
# Linux example
cp build/zephyr/zephyr.uf2 /media/${USER}/RPI-RP2

# MACOS example
cp build/zephyr/zephyr.uf2 /Volumes/RPI-RP2
```

The PI should reboot and immediately start running your uploaded
application.

## :white_check_mark: Testing

Zephyr provides a testing environment called `twister`.
This repo adds twister compliant testcases under `tests/`:

```bash
# In docker:
west twister -T tests -v --inline-logs
```

**note:** Make sure to install the necessary toolchain for a given test!
e.g. the basic tests require `x86_64-zephyr-elf`:

```bash
west sdk install -t x86_64-zephyr-elf
```
