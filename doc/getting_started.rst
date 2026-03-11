Getting Started
===============

Prerequisites
-------------

AURORA is built inside a ``west`` workspace.  The directory layout is::

   zephyr_workspace/
   ├── aurora/          ← this repository
   ├── modules/
   ├── .west/
   └── zephyr/

All ``west`` commands must be run from the ``aurora/`` directory.

Building
--------

Build the primary ``sensor_board`` application for one of the supported boards:

.. code-block:: shell

   # RP2040 (primary target)
   west build -b rpi_pico sensor_board

   # RP2350 RISC-V
   west build -b rpi_pico2_hazard3 sensor_board

   # ESP32-S3 custom board (Micrometer)
   west build -b esp32s3_micrometer/esp32s3/procpu sensor_board/

Build output is located at ``build/zephyr/zephyr.uf2`` and
``build/zephyr/zephyr.elf``.

Interactive Kconfig
^^^^^^^^^^^^^^^^^^^

.. code-block:: shell

   ./run.sh -b rpi_pico menuconfig

Docker Container
^^^^^^^^^^^^^^^^

.. code-block:: shell

   # Open a shell inside the dev container
   ./run.sh -b rpi_pico shell

   # Clean build artefacts
   ./run.sh clean

Flashing
--------

Via west
^^^^^^^^

.. code-block:: shell

   west flash

Via OpenOCD (CMSIS-DAP)
^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: shell

   sudo openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
     -c "adapter speed 5000" \
     -c "program build/zephyr/zephyr.elf verify reset exit"

Via USB (RPi Pico BOOTSEL mode)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Hold BOOTSEL while connecting the board, then copy the UF2 file:

.. code-block:: shell

   cp build/zephyr/zephyr.uf2 /media/${USER}/RPI-RP2
