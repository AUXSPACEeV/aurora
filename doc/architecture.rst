Architecture
============

AURORA runs on multiple interconnected PCBs that communicate over CAN bus.
The primary application is ``sensor_board``, which manages the IMU, barometric
sensor, flight state machine, and pyrotechnic ignition.

Threads
-------

``sensor_board`` uses three Zephyr threads (all priority 5, 2 KB stack):

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Thread
     - Guard
     - Purpose
   * - ``imu_task``
     - ``CONFIG_IMU``
     - Polls IMU at the configured frequency, updates orientation and
       acceleration globals.
   * - ``baro_task``
     - ``CONFIG_BARO``
     - Measures pressure/temperature, computes altitude.
   * - ``state_machine_task``
     - ``CONFIG_AURORA_STATE_MACHINE``
     - Runs at 10 Hz. Feeds sensor data into the state machine and fires pyro
       channels on state transitions.

Flight States
-------------

The simple state machine implements the following flight sequence::

   IDLE -> ARMED -> BOOST -> BURNOUT -> APOGEE -> MAIN -> REDUNDANT -> LANDED
                                                                    \-> ERROR

State transitions are driven by sensor thresholds configured via Kconfig
(boost acceleration, main descent height, apogee timeout, etc.).

Supported Boards
----------------

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Board
     - MCU
     - Notes
   * - ``sensor_board_v2/rp2040``
     - RP2040 (Cortex-M0+)
     - Primary target
   * - ``sensor_board_v2/rp2350a/hazard3``
     - RP2350 (RISC-V Hazard3)
     -
   * - ``esp32s3_micrometer/esp32s3/procpu``
     - ESP32-S3
     - Custom Auxspace board
