# `AURORA`

> **AU**xspace **RO**cket ope**RA**ting System

## General Info

### Disclaimer

This Project is a fork of the official
[Zephyr Example Project](https://github.com/zephyrproject-rtos/example-application)
with a few tweaks.

### Technical Terms

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
This project is specifically designed for the **METER-2** rocket,
incorporating a modular electronics stack for optimal performance and
flexibility.

The **AURORA** system leverages a series of interconnected PCBs
communicating over **CAN**, with each PCB housing a microchip running
the **AURORA** software.
The primary purpose of **AURORA** is to serve as avionics software,
managing essential data such as air pressure, acceleration, gyro, and
magnetometer readings.
Using the **Zephyr** kernel, **AURORA** handles task
scheduling and real-time operations, enabling reliable and efficient
communication between multiple **AURORA** instances through
**CAN**.

Join us as we continue to develop and refine **AURORA**,
contributing to the advancement of rocket technology and student
expertise in aerospace engineering!

## Setup

Setup Zephyr using either the docker container in this directory or follow the
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
from the Zephyr project.

### Native

First, create a workspace:

```bash
mkdir aurora_workspace && cd aurora_workspace
```

And add a python virtualenv to install west and other dependencies:

```bash
python3 -m venv .venv
source .venv/bin/activate

python3 -m pip install west
```

Also, don't forget the Zephyr SDK (it's big, so ensure you have enough free space):

```bash
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.3/zephyr-sdk-0.17.3_linux-x86_64.tar.xz
wget -O - https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.3/sha256.sum | shasum --check --ignore-missing
tar xvf zephyr-sdk-0.17.3_linux-x86_64.tar.xz
cd zephyr-sdk-0.17.3
./setup.sh -c -h

# Or alternatively only install for the target platform you desire (e.g. aarch64):
# ./setup.sh -c -h -t aarch64-zephyr-elf
```

When all dependencies are setup and ready, fetch the aurora sources:

```bash
west init -m https://github.com/AUXSPACEeV/aurora --mr maxist-develop workspace
cd workspace
west update
west zephyr-export

python3 -m pip install -r ./zephyr/scripts/requirements.txt
source ./zephyr/zephyr-env.sh
```

### Docker

<details> <summary> <b>Build Dependencies</b> (<i>click</i> to open) </summary>

If you don't want to install all Zephyr dependencies by yourself and system-wide,
this repository also comes with a configured docker container.

To install docker, head to the
[docker install instructions](https://docs.docker.com/engine/install/),
select your distro and follow the instructions.

[podman](https://podman.io/docs/installation)
also works for this project and can be selected using the `--engine`
option in the `run.sh` wrapper.

</details>

<details> <summary> <b>Container and Zephyr Workspace</b> (<i>click</i> to open) </summary>

After installing docker, all requirements are met to run the wrapper script:

```bash
# Open a shell in the development container for the rpi_pico board
./run.sh -b rpi_pico shell
```

The Zephyr workspace is configured to be at *$(pwd)/..*.
Zephyr itself is then found at *\<zephyr-workspace\>/zephyr* and this
application at *\<zephyr-workspace\>/\<aurora\>*.

*<zephyr-workspace>/<aurora>* is mounted into the container,
so changes that you perform inside the container will take effect in this
repository and vice-versa.

Run `west update` in *\<zephyr-workspace\>* to update modules.

**example**:

```bash
# Set up workspace directories
mkdir zephyr-workspace && cd zephyr-workspace

# Clone the project into the workspace
git clone https://github.com/AUXSPACEeV/aurora.git aurora
cd aurora

# Run the container
./run.sh -b rpi_pico shell
```

</details>

## Build

The following command is an example of how to use west when building an
application:

```bash
# Build the sensor_board project for the rpi pico
west build -b rpi_pico sensor_board
```

The output from the build will be at
*\<zephyr-workspace\>/\<aurora\>/build/zephyr*
called `zephyr.uf2` and `zephyr.elf`.

## Deployment

### West

Deploy the binary to your board by either using `west flash` or in case of the
Raspberry Pi Pico series, copy it onto the Pi's storage.

Currently, flashing from inside the docker container is untested and
unimplemented.

### Openocd

Natively, you can use openocd and gdb to flash and debug an application:

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

The RP2040 comes with a backup bootloader, in case the QSPI Flash is not
reachable by the ROM bootloader.
The `BOOTSEL` switch deasserts the QSPI CS pin and opens up the fallback
loader, which is a simple USB interface.
When connecting the PI to your PC in this state, the PI is registered as
an external drive called `RPI-RP2`, which can load `.uf2` files.

Copy the files from your drive to the volume like so:

```bash
# Linux example
cp build/zephyr/zephyr.uf2 /mount/${USER}/RPI-RP2
```

The PI should reboot and immediately start running your uploaded
application.
