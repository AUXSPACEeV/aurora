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

.. list-table::
   :header-rows: 1
   :widths: 25 45 30

   * - Name
     - UUID
     - Access
   * - Service
     - ``e8a59100-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - \-
   * - Board identifier
     - ``e8a59101-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read
   * - SM state
     - ``e8a59102-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - Raw sensors
     - ``e8a59103-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - Computed kinematics
     - ``e8a59104-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read, notify
   * - SM type
     - ``e8a59105-7c0e-4b5b-9a4c-1f1b6f7c4d70``
     - read

Read vs. notify
~~~~~~~~~~~~~~~

Every characteristic supports *read*: the central asks once and gets the
current value. The three live ones (state, raw sensors, computed) also
support *notify*: the central writes "1" to the characteristic's CCC
descriptor and then receives a push every time the rocket calls
``pad_link_publish_sm()``. Notifications are cheaper than polling at the
same rate and arrive immediately on each update.

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

Rocket-side integration
-----------------------

For the typical sensor-board application, the integration is two calls
in ``main.c`` and one Kconfig:

.. code-block:: c

   #include <aurora/lib/pad_link.h>
   #include <aurora/lib/state/state.h>

   int main(void)
   {
       /* ... other init ... */
       (void)pad_link_init();   /* non-fatal if it fails */
       return 0;
   }

   /* In the state-machine loop, right after sm_update(): */
   struct sm_inputs sm_in;
   sm_get_inputs(&sm_in);
   pad_link_publish_sm(sm_get_state(), sm_get_type(), &sm_in);

That is the whole rocket-side contract. The raw-sensor characteristic
fills itself in: the library subscribes to the IMU and baro zbus
channels internally and snapshots every published sample.

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
service UUID, reads the board id and SM type, then either subscribes
to notifications or polls reads on a timer:

.. code-block:: console

   $ pip install bleak
   $ python3 aurora/tools/pad_link_central_example.py
   connected to sensor_board_v2_rp2040 (sm_type=0)
   state: IDLE
   t=12345  alt=+0.1  v=+0.0  ypr=+0.3/-0.1/+0.0
   ...

The mode is selectable from the command line:

- ``--mode notify`` (default): subscribe to the SM-state and computed
  characteristics. The rocket pushes a notification on every SM tick.
  Lowest latency, highest radio traffic.
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
