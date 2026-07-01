Watchdog Supervision
====================

A flight computer must never wedge. If a thread deadlocks, spins on a stuck
sensor bus, or corrupts its own stack mid-flight, the vehicle still needs the
avionics to keep running the state machine and firing pyros. The watchdog
subsystem turns the SoC hardware watchdog into a last line of defence: if the
firmware stops making progress, the chip is reset and reboots into a known
state instead of hanging silently until the rocket hits the ground.

Why a software supervisor
-------------------------

Most SoC watchdogs expose a *single* hardware timer: you "feed" it
periodically and it resets the chip if a feed is ever missed. AURORA runs
several independent critical threads (Sensor polling, the state machine),
and any one of them stalling is a fault worth resetting for.
A single naive feed cannot tell whether *all* of those threads are healthy or
just the one that happens to call ``wdt_feed()``.

The subsystem solves this with a **software supervisor**:

- Each critical thread registers once and *checks in* on every loop
  iteration with :c:func:`aurora_wdt_feed`.
- A dedicated monitor thread wakes at a fixed interval and refreshes the
  hardware watchdog **only while every registered thread has checked in
  within its own deadline**.
- The moment one thread misses its deadline, the monitor stops feeding the
  hardware watchdog, which then resets the SoC.

This gives per-thread supervision with a single hardware channel and no
dynamic allocation. A thread that is merely slow is tolerated up to its
deadline; a thread that is truly stuck takes the whole system down and back
up cleanly.

.. note::

   The boot/init window is intentionally **not** supervised. Bringing up the
   SD card, Bluetooth and the sensors can take seconds and involves long
   blocking calls. Until the first thread registers, the monitor keeps the
   hardware watchdog fed unconditionally, so slow initialisation never causes
   a spurious reset. Supervision begins once each thread reaches its main
   loop and registers.

Failure escalation
------------------

There are two backstops, in order of severity:

1. **A supervised thread stalls.** The monitor detects the missed deadline,
   logs which task is responsible, and withholds the feed. On controllers
   with a warning stage (see below) a pre-reset log line names the culprit;
   the SoC then resets.
2. **The monitor itself is starved** (e.g. a runaway higher-priority thread
   busy-loops and never yields). The monitor never runs, so the hardware
   watchdog is never fed and resets the SoC directly. This is why the
   hardware watchdog, not just the software check, is the real safety net.

Hardware backends
-----------------

The subsystem is portable across the AURORA flight computers. It takes the
watchdog device from the ``auxspace,wdt`` devicetree chosen node and adapts to
the driver's capabilities:

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Board
     - Controller
     - Behaviour
   * - ``micrometer`` (ESP32-S3)
     - TIMG0 MWDT
     - Two stages: a warning interrupt fires one timeout period before the
       reset, so the culprit is flushed to the console first.
   * - ``sensor_board_v2`` (RP2040 / RP2350)
     - pico-watchdog
     - Reset-only. No warning stage: the SoC resets directly with no
       pre-reset log line. The pico-watchdog also caps the maximum timeout
       (~8.3 s on RP2040), so keep ``AURORA_WATCHDOG_TIMEOUT_MS`` well below
       that.

The library installs the timeout with a pre-reset callback and, if the driver
rejects it (``-ENOTSUP``), transparently retries without one. No board-specific
code is required in the application.

Devicetree
----------

Point the ``auxspace,wdt`` chosen node at the watchdog you want to use and
make sure that node is enabled. For the ESP32-S3:

.. code-block:: devicetree

   / {
      chosen {
         auxspace,wdt = &wdt0;
      };
   };

   &wdt0 {
      status = "okay";
   };

On the ``sensor_board_v2`` boards this wiring lives in the shared
``sensor_board_v2.dtsi`` so every RP2040/RP2350 revision inherits it.

Application integration
-----------------------

Three calls: arm the watchdog once in ``main()``, then register and feed from
each critical thread.

.. code-block:: c

   #include <aurora/lib/watchdog.h>

   int main(void)
   {
      /* Arm before the sensor/state threads reach their loops so their
       * check-ins keep it fed from the start. Non-fatal on failure.
       */
      if (aurora_wdt_init() != 0) {
         LOG_ERR("watchdog init failed; running unprotected");
      }
      /* ... other init ... */
      return 0;
   }

   void imu_task(void *, void *, void *)
   {
      /* ... blocking hardware init ... */

      /* Register once, right before the hot loop. */
      aurora_wdt_task_t wdt =
         aurora_wdt_register("imu", CONFIG_AURORA_WATCHDOG_IMU_DEADLINE_MS);

      while (1) {
         aurora_wdt_feed(wdt);   /* check in every iteration */
         imu_poll(imu0);
         k_sleep(K_MSEC(period));
      }
   }

If a thread should be paused (e.g. it enters a long, legitimately blocking
maintenance phase), call :c:func:`aurora_wdt_unregister` to drop it from
supervision and :c:func:`aurora_wdt_register` again afterwards.

Choosing deadlines
------------------

Two independent time budgets:

- ``AURORA_WATCHDOG_TIMEOUT_MS`` is the *hardware* budget: how long the
  monitor may go without feeding the hardware before it acts. It must be
  comfortably larger than ``AURORA_WATCHDOG_FEED_INTERVAL_MS`` so normal
  scheduling jitter never trips it, and (on the RP2 boards) below the
  controller's hardware maximum.
- The per-thread ``*_DEADLINE_MS`` symbols are the *software* budget: how
  long a given thread may go between check-ins. Set each one above that
  thread's normal loop period with margin. For a 100 Hz sensor poll (10 ms
  period) a 100 ms deadline absorbs transient stalls while still catching a
  real hang within a tenth of a second.

Because the state machine blocks waiting for sensor data, its deadline is
really a bound on "both sensor paths have gone silent" — in normal flight the
sensors stream continuously, so a long gap means the upstream threads have
already died.

Kconfig
-------

.. list-table::
   :header-rows: 1
   :widths: 50 15 35

   * - Symbol
     - Default
     - Purpose
   * - ``AURORA_WATCHDOG``
     - n
     - Enable the subsystem. Selects ``WATCHDOG``.
   * - ``AURORA_WATCHDOG_TIMEOUT_MS``
     - 2000
     - Hardware watchdog timeout. On the ESP32 MWDT the warning interrupt
       fires after this period and the reset one period later.
   * - ``AURORA_WATCHDOG_FEED_INTERVAL_MS``
     - 500
     - How often the monitor evaluates task health and feeds the hardware.
   * - ``AURORA_WATCHDOG_MAX_TASKS``
     - 4
     - Number of registration slots (one per critical thread).
   * - ``AURORA_WATCHDOG_MONITOR_STACK_SIZE``
     - 1024
     - Monitor thread stack size.
   * - ``AURORA_WATCHDOG_MONITOR_PRIORITY``
     - 3
     - Monitor thread priority. Should be higher (numerically lower) than the
       supervised threads so it is not starved by them.

The per-thread deadline symbols (``AURORA_WATCHDOG_IMU_DEADLINE_MS``,
``AURORA_WATCHDOG_BARO_DEADLINE_MS``, ``AURORA_WATCHDOG_SM_DEADLINE_MS``) are
application-level and live in the sensor-board ``Kconfig`` because they are
tied to the specific threads in ``main.c``.

API Reference
-------------

.. doxygengroup:: lib_watchdog
   :content-only:
