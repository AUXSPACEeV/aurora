# `AURORA`

> **AU**xspace **RO**cket ope**RA**ting System

## Introduction

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
Using the **FreeRTOS** kernel, **AURORA** handles task
scheduling and real-time operations, enabling reliable and efficient
communication between multiple **AURORA** instances through
**CAN**.

Join us as we continue to develop and refine **AURORA**,
contributing to the advancement of rocket technology and student
expertise in aerospace engineering!

## Building AURORA

<details> <summary> <b>Build Dependencies</b> (<i>click</i> to open) </summary>
The only dependency that is needed for <b>AURORA</b> to be built is
<b>Docker</b>.
</details>

Building AURORA works with the `run.sh` script in this git repositories
root directory:

```bash
./run.sh build
```

<details> <summary> <b>Build AURORA without the wrapper</b> (<i>click</i> to open) </summary>
The project can also be built by using <b>CMake</b> and <b>Make</b> in
the traditional way.
However, to do this, <b>pico-sdk</b> and <b>picotool</b> need to be
installed.

Once the environment is set up, you will need to add the following
environment variables and make sure they are visible to <b>CMake</b>:

```bash
# env
export PICO_SDK_PATH="${PATH_TO_PICO_SDK}"
export FREERTOS_KERNEL_PATH="$PATH_TO_SRC_KERNEL"  # optional

# prepare build
mkdir build
cd build
cmake -G "Unix Makefiles" ../src

# build
make
```

</details>

## Installing AURORA

After building the firmware, the build output is generated under `build/`.
There should be a file `aurora.uf2` which can be transferred onto the
board, either via USB or the RPI Debug Probe.

## License

This project is licensed under the MIT License - see the
[LICENSE](./LICENSE) file for details.

## Acknowledgements

This project relies on several tools and libraries that have greatly
contributed to its development:

* **picotool**: A tool for managing and interacting with Raspberry Pi Pico
devices.
* **FreeRTOS**: An open-source real-time operating system kernel used for
task scheduling.
* **pico-sdk**: The official SDK for programming Raspberry Pi Pico boards,
providing essential APIs and hardware support.
* **pico-project-generator**: A project generator for setting up new
Raspberry Pi Pico projects with ease.

Thank you to all the contributors and maintainers of these projects!

## Contact Information

For more information or inquiries, feel free to visit our website or
contact us via the following:

* **Website**: https://www.auxspace.de
* **Contact page**: https://auxspace.de/contact/
* **Email**:
    * **General Info**: info@auxspace.de
    * **License Info**: license@auxspace.de
    * **Software Info**: maximilian.stephan@auxspace.de

## Disclaimer

This project is maintained by students, for students.

<details> <summary> <b>Read more</b> (<i>click</i> to open) </summary>

AURORA is designed not to be the most abstract or highly efficient
software, but rather to be easily understandable and enjoyable to
develop.
The <b>Auxspace</b> team has experimented with several different
approaches to <b>AURORA</b>, including using the <b>Zephyr</b> or
<b>Embassy</b> operating systems and bare-metal programming.
Ultimately, the team decided on <b>FreeRTOS</b> as the operating system
kernel and the <b>pico-sdk</b> for its ease of use and well-documented
hardware support.

The primary purpose of this project is for the team to gain a deeper
understanding of low-level programming, real-time operating systems,
and general software development.
</details>

We are passionate about learning and warmly welcome any support or
assistance that comes our way!
