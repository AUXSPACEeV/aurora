Pad Link
========

The pad-link library turns the rocket into a small Bluetooth Low Energy
(BLE) peripheral so a nearby ground station (usually a launchrail
computer or a laptop running a BLE app) can read the live status while
the rocket is sitting on the launch pad.

It is **not** a flight downlink. The BLE radio range is tens of metres at
best, and once the motor lights, the rocket will leave that bubble
within a second or two. The link is meant for the minutes between
`firmware booted` and `launch button pressed`.

If you have used a fitness tracker or a smart watch, the design
is the same: the rocket *advertises* (broadcasts "I'm here"), the
ground station *scans* and *connects*, and then it reads or subscribes
to a handful of *characteristics* (think: named variables on the
device).

.. _pad-link-architecture-diagram:

.. image:: /img/pad_link_architecture.drawio.svg
   :alt: pad_link_arch

When to use it
--------------

Use the pad link when you want any of the following:

- Watch the state machine on the pad ("did it really go IDLE -> ARMED
  when I flipped the arm switch?") without a USB cable.
- Sanity-check the last raw IMU and baro readings before flight.
- Display computed altitude, velocity, and orientation on a phone or
  laptop during setup.
- Identify which board is on the pad (e.g. ``sensor_board_v2/rp2040``
  vs. an ESP32-S3 variant) when you have several flight computers in
  a backpack.

Do **not** use it for:

- In-flight telemetry. Use the telemetry library (HC-12 today, more
  long-range backends later). See :doc:`telemetry`.
- Sending commands to the rocket. The pad-link is read-only by design.
  The only way to influence the rocket from the ground is the arm
  switch and the launch wire.

Connection lifecycle
--------------------

The rocket is the *peripheral*; the ground station is the *central*.
The lifecycle is symmetric and forgiving:

1. ``pad_link_init()`` brings up the BLE host stack and starts
   connectable advertising.
2. Any central that scans for the advertised name (default
   ``AURORA-Rocket``) sees the rocket and may connect.
3. While connected, the central reads characteristics directly or
   subscribes to notifications and is pushed every new value.
4. When the central disconnects, deliberately, or because the rocket
   just travelled 200 m up in two seconds, the disconnect
   callback re-starts advertising. No state on the rocket cares
   whether anyone is listening.

The flight code never waits on the link. A central showing up an hour
after boot, or never showing up at all, is the same code path for the
rocket.

GATT service
------------

All data is grouped under one custom *primary service*. A service is
just a container. The interesting parts are its *characteristics*.
You can think of each characteristic as a named, typed register the
central can read.

128-bit UUIDs are used throughout. The service UUID lets a central
filter scan results to "AURORA rockets only":

All UUIDs share the base ``e8a591xx-7c0e-4b5b-9a4c-1f1b6f7c4d70``; the
low byte ``xx`` identifies the characteristic.

.. list-table::
   :header-rows: 1
   :widths: 25 10 35 30

   * - Name
     - UUID ``xx``
     - Full UUID
     - Access
   * - Service
     - ``00``
     - ``e8a59100-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - \-
   * - Board identifier
     - ``01``
     - ``e8a59101-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read
   * - SM state
     - ``02``
     - ``e8a59102-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - Raw sensors
     - ``03``
     - ``e8a59103-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify *(deprecated)*
   * - Computed kinematics
     - ``04``
     - ``e8a59104-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - SM type
     - ``05``
     - ``e8a59105-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read
   * - Board capabilities
     - ``a0``
     - ``e8a591a0-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read
   * - Barometer
     - ``a1``
     - ``e8a591a1-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - Accelerometer
     - ``a2``
     - ``e8a591a2-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - Gyrometer
     - ``a3``
     - ``e8a591a3-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - 6-DoF IMU
     - ``a4``
     - ``e8a591a4-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - 9-DoF IMU
     - ``a5``
     - ``e8a591a5-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - planned
   * - GPS/GNSS
     - ``a6``
     - ``e8a591a6-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - planned
   * - Inner temperature
     - ``a7``
     - ``e8a591a7-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - Motor temperature
     - ``a8``
     - ``e8a591a8-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - planned
   * - Hull temperature
     - ``a9``
     - ``e8a591a9-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - planned

"Planned" characteristics have UUID defines reserved in ``pad_link.c``
but no GATT table entry yet; their corresponding bits in the boardcap
register remain ``0`` until the sensor source is wired up.

Read vs. notify
~~~~~~~~~~~~~~~

Every characteristic supports *read*: the central asks once and gets the
current value. The characteristics marked "read, notify" also support
*notify*: the central writes "1" to the characteristic's CCC descriptor
and then receives a push every time the rocket updates the snapshot.
Notifications are cheaper than polling at the same rate and arrive
immediately on each update.

Board capabilities
~~~~~~~~~~~~~~~~~~

After connecting, read characteristic ``a0`` (boardcap) once. It is a
``uint32_t`` little-endian register that describes which sensor
characteristics carry valid data on this specific board. The
application declares the value during init via ``pad_link_set_caps()``,
composing it from the ``PL_CAP_*`` flags in
``include/aurora/lib/pad_link.h`` — which sensors a board carries is
hardware knowledge the library does not guess at.

The register is split into four byte-sized groups:

**Byte 0 — IMU group**

.. list-table::
   :header-rows: 1
   :widths: 15 25 60

   * - Bits
     - Name
     - Description
   * - ``[2:0]``
     - IMU type
     - ``0`` = none, ``1`` = 6-DoF (accel + gyro), ``2`` = 9-DoF (accel + gyro + mag)
   * - ``[3]``
     - Accel
     - 1 = accelerometer data valid (characteristic ``a2``)
   * - ``[4]``
     - Gyro
     - 1 = gyrometer data valid (characteristic ``a3``)
   * - ``[7:5]``
     - —
     - reserved

**Byte 1 — Environmental group**

.. list-table::
   :header-rows: 1
   :widths: 15 25 60

   * - Bits
     - Name
     - Description
   * - ``[8]``
     - Baro
     - 1 = barometer data valid (characteristic ``a1``)
   * - ``[9]``
     - Inner temp
     - 1 = inner temperature data valid (characteristic ``a7``)
   * - ``[10]``
     - Motor temp
     - 1 = motor temperature data valid (characteristic ``a8``, planned)
   * - ``[11]``
     - Hull temp
     - 1 = hull temperature data valid (characteristic ``a9``, planned)
   * - ``[15:12]``
     - —
     - reserved

**Byte 2 — Positioning group**

.. list-table::
   :header-rows: 1
   :widths: 15 25 60

   * - Bits
     - Name
     - Description
   * - ``[16]``
     - GPS/GNSS
     - 1 = GPS data valid (characteristic ``a6``, planned)
   * - ``[23:17]``
     - —
     - reserved

**Byte 3 — reserved**

.. list-table::
   :header-rows: 1
   :widths: 15 25 60

   * - Bits
     - Name
     - Description
   * - ``[31:24]``
     - —
     - reserved

The IMU type field uses the ``pl_cap_imu_type`` C enum defined in
``lib/pad_link/pad_link_wire.h`` (``PL_CAP_IMU_TYPE_NONE``,
``PL_CAP_IMU_TYPE_6DOF``, ``PL_CAP_IMU_TYPE_9DOF``). The Python
example script mirrors these as ``CAP_IMU_TYPE_*`` constants.

Characteristics in detail
~~~~~~~~~~~~~~~~~~~~~~~~~

**Board identifier**: UTF-8 string, length-bounded by
``CONFIG_AURORA_PAD_LINK_BOARD_ID``. Defaults to the Zephyr board name
(e.g. ``sensor_board_v2_rp2040``). Read once; it doesn't change.

**SM type**: ``uint8_t``. Identifies *which* state machine
implementation the firmware is running, so the central knows how to
decode the SM-state byte. ``0`` is the simple 9-state SM
(see :doc:`state`); future implementations append new values. Read
once after connecting.

**SM state**: ``uint8_t``. Current flight state, as defined by the
active SM implementation. With ``SM type = 0`` (simple), the byte maps
to ``IDLE, ARMED, BOOST, BURNOUT, APOGEE, MAIN, REDUNDANT, LANDED,
ERROR``.

**Raw sensors**: packed, little-endian struct. Most recent samples
from the IMU and barometer in their native Zephyr ``sensor_value``
format (``val1`` = whole part, ``val2`` = micro-fraction).

.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``uptime_ms`` of the latest contributing sample
   * - 4
     - 12
     - ``i32[3]``
     - ``accel.val1`` (x, y, z)
   * - 16
     - 12
     - ``i32[3]``
     - ``accel.val2`` (x, y, z)
   * - 28
     - 12
     - ``i32[3]``
     - ``gyro.val1`` (x, y, z)
   * - 40
     - 12
     - ``i32[3]``
     - ``gyro.val2`` (x, y, z)
   * - 52
     - 4
     - ``i32``
     - ``temperature.val1``
   * - 56
     - 4
     - ``i32``
     - ``temperature.val2``
   * - 60
     - 4
     - ``i32``
     - ``pressure.val1``
   * - 64
     - 4
     - ``i32``
     - ``pressure.val2``

To recover a physical value, combine the two halves:
``value = val1 + val2 * 1e-6``.

**Computed kinematics**: packed, little-endian struct. The outputs
of the state-machine input pipeline (after Kalman filtering, if
enabled). 32-bit floats keep the payload small; the precision is far
more than the central needs for a status screen.

.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``uptime_ms``
   * - 4
     - 4
     - ``f32``
     - ``altitude`` (m)
   * - 8
     - 4
     - ``f32``
     - ``velocity`` (m/s, vertical)
   * - 12
     - 4
     - ``f32``
     - ``yaw`` (deg)
   * - 16
     - 4
     - ``f32``
     - ``pitch`` (deg)
   * - 20
     - 4
     - ``f32``
     - ``roll`` (deg)
   * - 24
     - 4
     - ``f32``
     - ``accel_vert`` (m/s², world-frame, gravity-removed)

All sensor characteristics below use *micro-units*: divide the raw
``int64_t`` value by 1 000 000 to recover the physical quantity.

**Board capabilities** (``a0``): ``uint32_t`` LE, 4 bytes.
See `Board capabilities`_ above.

**Barometer** (``a1``): 20 bytes. Present when ``PL_CAP_BARO`` is set.

.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``uptime_ms``
   * - 4
     - 8
     - ``i64``
     - ``temp_us`` (µ°C)
   * - 12
     - 8
     - ``i64``
     - ``press_us`` (µPa)

**Accelerometer** (``a2``): 28 bytes. Present when ``PL_CAP_ACCEL`` is set.

.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``uptime_ms``
   * - 4
     - 24
     - ``i64[3]``
     - ``accel_us`` x, y, z (µm/s²)

**Gyrometer** (``a3``): 28 bytes. Present when ``PL_CAP_GYRO`` is set.

.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``uptime_ms``
   * - 4
     - 24
     - ``i64[3]``
     - ``gyro_us`` x, y, z (µrad/s)

**6-DoF IMU** (``a4``): 52 bytes. Present when IMU type ≥ 6-DoF.
Combines ``a2`` and ``a3`` in one characteristic — subscribe to ``a4``
*or* ``a2`` + ``a3``, not both.

.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``uptime_ms``
   * - 4
     - 24
     - ``i64[3]``
     - ``accel_us`` x, y, z (µm/s²)
   * - 28
     - 24
     - ``i64[3]``
     - ``gyro_us`` x, y, z (µrad/s)

**Inner temperature** (``a7``): 12 bytes. Present when ``PL_CAP_TEMP_INNER``
is set. Sourced from the barometer sensor's die temperature.

.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``uptime_ms``
   * - 4
     - 8
     - ``i64``
     - ``temp_us`` (µ°C)

Rocket-side integration
-----------------------

For the typical application, the integration is three calls and one
Kconfig:

.. code-block:: c

   /* main.c — one-time init */
   #include <aurora/lib/pad_link.h>

   int main(void)
   {
       /* ... other init ... */

       /* Declare this board's sensor fit (hardware knowledge the
        * library does not guess at), then bring up the BLE stack.
        */
       pad_link_set_caps(PL_CAP_IMU_TYPE(PL_CAP_IMU_TYPE_6DOF) |
                         PL_CAP_ACCEL | PL_CAP_GYRO |
                         PL_CAP_BARO | PL_CAP_TEMP_INNER);
       (void)pad_link_init();   /* non-fatal if it fails */
       return 0;
   }

   /* In the state-machine loop, right after sm_update(): */
   struct sm_inputs sm_snap;

   sm_get_inputs(&sm_snap);
   pad_link_publish_sm(sm_get_state(), sm_get_type(), &sm_snap);

``pad_link_publish_sm()`` updates the state and computed-kinematics
characteristics and fires notifications for any subscribed central. The
sensor characteristics fill themselves in automatically: the library
subscribes to the IMU and baro ZBUS channels and snapshots every
published sample.

Kconfig
-------

.. list-table::
   :header-rows: 1
   :widths: 50 20 30

   * - Symbol
     - Default
     - Purpose
   * - ``AURORA_PAD_LINK``
     - n
     - Enable the library. Pulls in ``BT`` and ``BT_PERIPHERAL``.
   * - ``AURORA_PAD_LINK_DEVICE_NAME``
     - ``"AURORA-Rocket"``
     - Name in the advertising payload: what the central displays
       when scanning.
   * - ``AURORA_PAD_LINK_BOARD_ID``
     - ``"$(BOARD)"``
     - Value of the board-identifier characteristic. Override per
       hardware revision.

A board configuration also has to enable a working BLE controller for
the chip in question (Bluetooth HCI driver, controller stack, …). Those
configs live in the board ``.conf`` files because they vary per SoC.

Ground-station example
----------------------

The central side is whatever can speak BLE: a phone running ``nRF
Connect``, a laptop with ``bluetoothctl``, the launchrail's own
firmware, or a small Python script. The shape of the code is always
the same: scan, connect, discover, then read or subscribe.

A runnable Python reference using `Bleak <https://bleak.readthedocs.io>`_
ships in-tree as :doc:`/tools/pad_link_central_example`. It scans by
service UUID, reads the board id, SM type, and boardcap register, prints
a capability summary, then subscribes to or polls only the sensor
characteristics that are actually present on the board:

.. code-block:: console

   $ pip install bleak
   $ python3 aurora/tools/pad_link_central_example.py
   connected to sensor_board_v2_esp32s3 (sm_type=0)
   capabilities:
     IMU:        6-DoF (accel + gyro)
     Barometer:  present
     Inner temp: present
     Motor temp: not present
     Hull temp:  not present
     GPS/GNSS:   not present
   state: IDLE
   baro: temp=22.10°C  press=101325.00 Pa
   imu6: a=[x=+0.000, y=+0.000, z=-9.810] m/s²  g=[x=+0.000, y=+0.000, z=+0.000] rad/s
   t=12345  alt=+0.1  v=+0.0  ypr=+0.3/-0.1/+0.0
   ...

The mode is selectable from the command line:

- ``--mode notify`` (default): subscribe to state, computed kinematics,
  and any sensor characteristics enabled by boardcap. The rocket pushes
  a notification on every SM tick. Lowest latency, highest radio traffic.
- ``--mode poll --interval 1.0``: skip notifications and ``read`` the
  characteristics on a timer. The central drives the cadence; the
  rocket never pushes. Use this for a low-rate status display where
  every SM-tick update would be wasted.
- ``--duration`` caps how long the script stays connected (default
  60 s).

Read the file itself for the wire-format decoding. The header
docstring lists prerequisites, gotchas, and pointers on extending it
into a fuller dashboard.

A few notes for the first time you wire this up:

- BLE characteristic reads are limited by the negotiated *MTU*. With
  the default 23-byte ATT MTU the raw-sensor (68 B) and computed
  (28 B) payloads still arrive correctly, because the central
  transparently issues *long reads*. Negotiating a larger MTU
  (``BleakClient(..., mtu=247)``) makes notifications and reads a
  single round-trip and is worth doing.
- The first read of ``sm_state`` *before* the state-machine has run
  even once returns 0 (``IDLE``). The values stabilise within
  milliseconds of boot.
- Reconnecting after a disconnect just works: the rocket re-advertises
  immediately. Centrals typically remember the address and reconnect
  on the next scan.

Failure modes
-------------

The library is designed so nothing it does can break the flight code:

- ``bt_enable()`` returns an error (no controller, bad config, …)
  -> ``pad_link_init()`` logs and returns the error. ``main()`` ignores
  it. The flight loop runs normally; you simply won't see any
  advertising.
- Notification send fails (queue full, central too slow)
  -> the failure is silently dropped. The next call to
  ``pad_link_publish_sm()`` will try again with fresher data.
- Central disconnects mid-flight
  -> ``disconnected`` callback re-arms advertising. The next time a
  central is in range, it can reconnect. The flight loop is
  unaffected.

There is intentionally no retry logic, no buffering for offline
centrals, and no fail-safe mode. The pad link is allowed to be
missing, reconnecting and imperfect.
That is what the SD-card log and the HC-12 downlink are for.

API Reference
-------------

.. doxygengroup:: lib_pad_link
   :content-only:
