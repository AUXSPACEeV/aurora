# `zephyr-example-setup`

> Example setup of applications using the Auxspace Zephyr port

## General Info

### Technical Terms

* `RTOS`: **R**eal-**T**ime **O**perating **S**ystem
* `µC`: **Mi**cro**C**ontroller (µ - Controller)
* `MCU`: **M**icro**C**ontroller **Unit**
* `SOC`: **S**ystem **O**n **C**hip
* `Zephyr`: Open-Source RTOS Kernel for a large variety of MCUs and SOCs
* `fork`: A *forked* repository is a direct clone of another, with custom
modifications made
* `Docker`: "Docker Engine is an open source containerization technology
for building and containerizing your applications"
(<https://docs.docker.com/engine/>)
* `device-tree`: Tree-like configuration for hardware integration

### Project Information

This Project is a fork of the official
[Zephyr Example Project](https://github.com/zephyrproject-rtos/example-application)
with a few tweaks.
All changes made in comparison to the zephyr example application are
traceable through the well-documented git history of this project.
The original README.md from the upstream repository has been moved to
[doc/README.md](./doc/README.md) for the sole purpose of having an
easier and more specific entrypoint for newcomers and first semester
students with this Auxspace-specific document.

As the title of this repository suggests, the application is using zephyr
as an RTOS.
The Zephyr Kernel acts **application-centric**, which means that
the application implements the main entrypoint and includes the
Zephyr Kernel.
Zephyr supports memory protection, simultaneous multi-processing
(`SMP`) and many more useful features.
Integrating custom boards is kept straighforward with
device-tree and Kconfig hardware configuration, similar
to the [Linux Kernel](https://github.com/torvalds/linux).

## Setup

Setup Zephyr using either the docker container in this directory or follow the
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
from the Zephyr project.

### Docker

If you don't want to install all Zephyr dependencies by yourself and system-wide,
this repository also comes with a configured docker container.

To install docker, head to the
[docker install instructions](https://docs.docker.com/engine/install/),
select your distro and follow the instructions.

[podman](https://podman.io/docs/installation)
should also work in this project and can be selected using the `-e`
option in the `run.sh` wrapper.

### Container and Zephyr Workspace

After installing docker, all requirements are met to run the wrapper script:

```bash
# Open a shell in the development container
./run.sh shell
```

The Zephyr workspace is configured to be at */builder/zephyr-workspace*.
Zephyr itself is then found at */builder/zephyr-workspace/zephyr* and this
application at */builder/zephyr-workspace/zephyr-example-setup*.

*/builder/zephyr-workspace/zephyr-example-setup* is mounted into the container,
so changes that you perform inside the container will take effect in this
repository and vice-versa.

Run `west update` in */builder/zephyr-workspace* to update modules.

## Build

```bash
cd /builder/zephyr-workspace/zephyr-example-setup

# Build the project for the m33 core on the rpi pico 2 (example)
west build -b rpi_pico2/rp2350a/m33 app
```

The output from the build will be at
*/builder/zephyr-workspace/zephyr-example-setup/build/zephyr*
called `zephyr.uf2`.

## Deployment

Deploy the binary to your board by either using `west flash` or in case of the
Raspberry Pi Pico series, copy it onto the Pi's storage.

Currently, flashing from inside the docker container is untested and
might need patching.
