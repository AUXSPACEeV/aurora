Watchdog Recovery
=================

.. note::

   "Recovery" here means **watchdog recovery** of the firmware's flight state,
   resuming the state machine after a reset. It is unrelated to the vehicle's
   physical *recovery* system (parachutes / pyros). The source, Kconfig symbol
   and API are all named ``wdt_recovery`` to keep that distinction clear, and
   the source lives alongside the watchdog library in ``lib/watchdog/``.

When the :doc:`watchdog` resets a mid-flight board, the default outcome is a
cold reboot into ``SM_IDLE``. The flight computer forgets it was ever flying.
For a rocket already past liftoff that is the wrong answer: it will sit in
``IDLE`` waiting to be armed while it is actually descending. The wdt_recovery
subsystem lets the board reboot *and resume the flight* from the state it was
in, so the state machine keeps driving the flight sequence instead of
starting over.

What is persisted
-----------------

Almost nothing new. The raw sensor stream and the state audit are already on
the SD card, so the record recovery keeps in fast, always-available storage is
just:

- the **last flight state** (which ``SM_*`` the machine was in),
- a one-bit **"watchdog imminent" marker** the supervisor raises just before it
  lets the chip reset, and
- the **barometric ground-reference pressure**.

The last one matters more than it looks. Altitude is computed AGL relative to a
ground reference captured on the *first* baro sample after boot, so a naive
reboot would re-zero the frame at whatever altitude the rocket happens to be,
and the state machine's altitude thresholds (boost altitude ``T_H``, main-deploy
height ``T_M``) would then be evaluated in a shifted frame. Restoring the
pre-reset reference before the first post-reboot sample keeps altitude in the
same frame across the reset. The Kalman filter itself is deliberately **not**
persisted; see below.

That record is a handful of bytes. It is written on every state transition, a
rare event, a few times per flight, so there is no polling or high-rate
persistence involved.

Where it is stored
------------------

The record lives in a **retained-memory** area selected by the
``auxspace,recovery`` devicetree chosen node, wrapped by Zephyr's retention
subsystem (magic prefix + checksum). On the ESP32-S3 that area is a slice of
**RTC fast RAM**, which has a deliberately useful property:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Event
     - Retained record
   * - Watchdog / warm reset (power maintained)
     - **Survives** → eligible for recovery
   * - Full power cycle / brown-out
     - **Cleared** → cold boot, no recovery

This is exactly the semantics a flight computer wants. A watchdog reset in the
air keeps power, so the record survives and the flight resumes. A board that
was switched off and on again on the ground comes up clean in ``IDLE``.
It will never resurrect a stale ``BOOST`` or ``APOGEE`` state and surprise
someone on the pad.
The checksum means a cold boot, a corrupted region, or a board with no recovery
backend all degrade safely to "no recovery".

Detecting a watchdog reboot
---------------------------

"Was the last reset a watchdog reset?" is answered from two independent
signals, because no single one covers every board:

1. **Hardware reset cause** (``hwinfo``). The ESP32-S3 and RP2350 latch a
   ``RESET_WATCHDOG`` cause; the RP2040 does **not** expose a watchdog bit.
2. **Software marker**. The watchdog supervisor calls
   :c:func:`aurora_wdt_reset_imminent` the moment it decides to withhold the
   feed, and the wdt_recovery library stamps the record. This covers the RP2040
   and corroborates the others.

If either signal indicates a watchdog reset **and** the saved state is worth
resuming (not ``SM_IDLE`` or ``SM_ERROR``), recovery is latched at boot.

.. note::

   The one gap is a *starved supervisor* on the RP2040: if a runaway
   high-priority thread prevents the monitor from ever running, it cannot raise
   the software marker, and the RP2040 has no hardware watchdog cause to fall
   back on. That specific reset will not be recognised as a watchdog reset on
   that chip. The ESP32-S3 and RP2350 are unaffected (hardware cause).

Boot flow
---------

.. code-block:: text

   power on ─▶ aurora_wdt_recovery_init()
                 │  read hwinfo reset cause
                 │  read + validate retained record
                 ▼
        watchdog reset AND state ∉ {IDLE, ERROR}?
             │ yes                        │ no
             ▼                            ▼
     latch pending recovery        boot normally (IDLE)

   state machine thread:
     sm_init()
     if aurora_wdt_recovery_pending(&s):
         sm_restore_state(s)   # resume the flight

State restoration
-----------------

:c:func:`sm_restore_state` forces the machine into the saved state. States
whose exit is gated by a timeout (``APOGEE``, ``MAIN``, ``REDUNDANT``) get that
timeout restarted from the moment of recovery, so the sequence can still
complete on live sensor data. Condition-driven transitions (altitude, velocity)
resume naturally from the incoming sensor stream.

Only *control-flow* state is restored. Hardware side effects that already
happened before the reset are **not** replayed:

- **Pyros are not re-fired.** The application seeds the pyro bookkeeping to the
  recovered state, so a channel deployed before the reset is not triggered
  again on boot. Future transitions (e.g. into ``REDUNDANT``) still fire
  normally. The flip side is that hardware charging state performed by a
  *skipped* transition is not reconstructed; review the pyro sequence for your
  vehicle if you rely on mid-sequence charging.
- **The Kalman filter restarts cold** and re-converges from the first samples
  after reboot. This is deliberate, not an omission. Persisting it would be
  worse than useless: its stored altitude is in the *old* frame (the ground
  reference is what we persist instead), the multi-second reboot gap makes the
  state stale, and re-injecting a confident-but-wrong altitude could make the
  filter's innovation gate reject the correct fresh baro measurements. A cold
  start with high initial covariance re-locks altitude within about a second in
  the restored frame; only vertical velocity has a brief convergence transient.

.. note::

   That velocity transient is the one rough edge: a freshly re-initialised
   filter starts at ``velocity = 0``, so recovering directly into ``REDUNDANT``
   can briefly satisfy the landing condition (``|velocity|`` below ``T_L``)
   before the estimate re-converges, risking a premature ``LANDED``. It is not
   dangerous (the recovery charge is already deployed by then), just early. If
   that matters for your vehicle, gate velocity-driven transitions for a short
   settling window after recovery.

Application integration
-----------------------

.. code-block:: c

   #include <aurora/lib/wdt_recovery.h>

   int main(void)
   {
      /* First thing: resolve the reboot reason and latch any recovery. */
      (void)aurora_wdt_recovery_init();
      /* ... watchdog + other init ... */
      return 0;
   }

   /* State machine thread, right after sm_init(): */
   enum sm_state s;
   if (aurora_wdt_recovery_pending(&s)) {
      sm_restore_state(s);
      prev_state = s;
      pyro_state = s;   /* suppress re-firing already-deployed pyros */
   }

   /* On every state transition: */
   aurora_wdt_recovery_save_state(new_state);

Devicetree
----------

Point ``auxspace,recovery`` at a ``zephyr,retention`` node backed by retained
memory. On the ESP32-S3 the RTC fast RAM already exists as a memory region, so
it only needs the retention wrapper:

.. code-block:: devicetree

   / {
      chosen {
         auxspace,recovery = &retention0;
      };
   };

   &rtc_fast_ram {
      status = "okay";
      #address-cells = <1>;
      #size-cells = <1>;

      retainedmem0: retainedmem {
         compatible = "zephyr,retained-ram";
         status = "okay";
         #address-cells = <1>;
         #size-cells = <1>;

         retention0: retention@0 {
            compatible = "zephyr,retention";
            status = "okay";
            reg = <0x0 0x20>;   /* 32 bytes: prefix + checksum + record */
            prefix = [41 58];   /* "AX" magic */
            checksum = <1>;
         };
      };
   };

On a board without a dedicated always-on RAM (e.g. the RP2040/RP2350), the wdt
stays unimplemented.
Without the chosen node the subsystem cannot be enabled and the board simply
boots cold every time. The safe default.

Kconfig
-------

.. list-table::
   :header-rows: 1
   :widths: 45 15 40

   * - Symbol
     - Default
     - Purpose
   * - ``AURORA_WDT_RECOVERY``
     - n
     - Enable the subsystem. A sub-option of ``AURORA_WATCHDOG``; also depends
       on a ``zephyr,retention`` node and selects ``RETENTION``,
       ``RETAINED_MEM`` and ``HWINFO``.

API Reference
-------------

.. doxygengroup:: lib_wdt_recovery
   :content-only:
