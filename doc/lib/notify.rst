Notifications
=============

The notification library provides an abstract interface for user-facing
indicators (buzzer, RGB LED, ...).
Each backend registers a :c:struct:`notify_backend` at link time via an iterable
section.
The library fans out every call to all enabled backends.

Backends
--------

- **PWM Buzzer** (``CONFIG_AURORA_NOTIFY_BUZZER``): drives a passive
  buzzer via PWM to signal boot, state changes, and errors. Runs on
  a dedicated worker thread so that the blocking tone sequences do
  not stall the caller (typically the state-machine task).
  See `Buzzer Patterns`_ and `Threading and Queueing`_.
- **PWM LED** (``CONFIG_AURORA_NOTIFY_LED``): drives an LED via PWM
  to signal boot, state changes, and errors. LED does not blink when data logger
  is disabled. See `LED Patterns`_.

Notification Patterns
---------------------

The following tables describe exactly what each backend does for every
event in the notification API. Patterns are kept short and distinctive so
operators on the launch pad can identify system state by ear and eye alone.

The buzzer "alphabet" encodes meaning along three axes so patterns are
hard to confuse:

- **Pitch register**: low (safe / pre-flight) up to top (recovery /
  alarm).
- **Beep count**: one event stands alone; a second, then a third beep
  mark escalating recovery stages (``MAIN`` => ``REDUNDANT``).
- **Pitch direction**: a *rising* run is a positive "go" signal
  (flight-ready, log saved); a *falling* run is a stand-down / safe
  signal (disarm).

Signal Reference
~~~~~~~~~~~~~~~~

At-a-glance mapping of every notified event to its buzzer and LED
signature. This is the operator-facing cheat sheet; the per-backend
tables below give the precise timings.

.. list-table::
   :header-rows: 1
   :widths: 22 34 26 18

   * - Event / State
     - Buzzer
     - LED
     - Meaning
   * - Boot
     - 1 · 700 Hz · 150 ms (low blip)
     - Solid ON 500 ms, then OFF
     - Power applied, notification stack up.
   * - Flight-ready / Calibration complete
     - Rising chime 1400 => 2100 => 2800 Hz, 120 ms each
     - 2 quick flashes (60 / 60 ms)
     - IMU calibration finished; rocket is flight-ready.
   * - ARM (=> ``ARMED``)
     - 2 · 2100 Hz · 180 ms (120 ms gap)
     - Even blink 200 / 200 ms
     - Armed, awaiting launch. **Pyros live.**
   * - DISARM (=> ``IDLE``)
     - Falling 2100 => 1400 Hz, 150 ms each
     - Slow heartbeat 60 / 940 ms
     - Stood down, safe.
   * - State change (``BOOST`` / ``BURNOUT``)
     - 1 · 1400 Hz · 90 ms (neutral tick)
     - Off (dark ascent)
     - Nominal in-flight transition.
   * - Apogee (=> ``APOGEE``)
     - 1 · 2800 Hz · 500 ms (long high tone)
     - Blink 500 / 500 ms
     - Apogee detected, drogue event.
   * - Main (=> ``MAIN``)
     - 2 · 2800 Hz · 250 ms (150 ms gap)
     - Blink 250 / 250 ms
     - Main parachute deployment.
   * - Redundant (=> ``REDUNDANT``)
     - 3 · 3500 Hz · 200 ms (150 ms gap)
     - Fast flicker 80 / 80 ms
     - Backup deployment: one more, higher beep than ``MAIN``.
   * - Landed (=> ``LANDED``)
     - Chosen melody (looping)
     - Slow beacon 700 / 300 ms
     - Flight complete; audible/visible recovery beacon.
   * - Flight log written
     - Rising 1400 => 3500 Hz "ta-daa" (120 + 280 ms)
     - 3 quick flashes (80 / 80 ms)
     - Flight record finalised on the filesystem.
   * - Error
     - 5 · 4200 Hz · 100 ms (100 ms gaps)
     - Solid ON
     - Unrecoverable error. Service required.

Events
~~~~~~

These are the events (hooks) dispatched by the notification library.
Every registered backend reacts independently; not every backend reacts
to every event.

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Event
     - When it fires
   * - ``on_boot``
     - Once at system startup, after backends are initialised.
   * - ``on_calibration_complete``
     - IMU calibration has finished and the rocket is flight-ready
       (also the flight-readiness signal).
   * - ``on_state_change``
     - Flight state-machine transition (see state table below).
   * - ``on_log_written``
     - A flight log has been written to the filesystem, i.e. the
       post-flight conversion of the recording has completed.
   * - ``on_error``
     - An unrecoverable error condition was reported.
   * - ``on_powerfail``
     - A power failure was detected, or the system recovered from one.

.. _led-patterns:

LED Patterns
~~~~~~~~~~~~

The LED backend drives every LED child of the ``auxspace_led`` chosen
node in lockstep (same pattern on all LEDs). Brightness is 100%
(``MAX_BRIGHTNESS``) whenever the LED is lit. Sustained states use a
continuous blink handed to the ``pwm-leds`` driver (non-blocking);
discrete events use short one-shot flash bursts.

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Event / State
     - Pattern
     - Meaning
   * - Boot
     - Solid ON for 500 ms, then OFF
     - System powered up and notification stack initialised.
   * - Calibration complete / Flight-ready
     - 2 quick flashes (60 ms ON / 60 ms OFF)
     - IMU calibration finished, rocket flight-ready.
   * - Flight log written
     - 3 quick flashes (80 ms ON / 80 ms OFF)
     - Flight record finalised on the filesystem.
   * - State => ``IDLE``
     - Slow heartbeat 60 ms ON / 940 ms OFF (~1 Hz)
     - Safe, disarmed. Awaiting arm command.
   * - State => ``ARMED``
     - Even blink 200 ms ON / 200 ms OFF (~2.5 Hz)
     - Armed and awaiting launch detection. **Pyros live.**
   * - State => ``APOGEE``
     - Blink 500 ms ON / 500 ms OFF (slow strong pulse)
     - Apogee detected, drogue event.
   * - State => ``MAIN``
     - Blink 250 ms ON / 250 ms OFF (medium pulse)
     - Main parachute deployment.
   * - State => ``REDUNDANT``
     - Blink 80 ms ON / 80 ms OFF (fast flicker)
     - Backup deployment active.
   * - State => ``LANDED``
     - Blink 700 ms ON / 300 ms OFF (slow beacon)
     - Flight complete, rocket on ground. Safe to recover.
   * - State => ``ERROR``
     - Solid ON
     - Unrecoverable error. Service required.
   * - ``BOOST`` / ``BURNOUT``
     - All LEDs OFF
     - Dark ascent: LEDs are silent during boost to save power and
       avoid optical noise. Apogee/main/redundant re-light briefly to
       mark the recovery events.
   * - Powerfail (loss)
     - All LEDs OFF; subsequent state/error events suppressed
     - Power dropped below threshold; backend is muted until recovery
       to conserve the backup rail.
   * - Powerfail (recover)
     - LEDs resume normal behaviour on the next event
     - Main power restored.

.. note::
   When a powerfail has been signalled and not yet recovered, the LED
   backend ignores state changes, calibration, log-written and error
   events. It will re-enable itself on the next event after
   ``on_powerfail(recover=1)``.

.. _buzzer-patterns:

Buzzer Patterns
~~~~~~~~~~~~~~~

The buzzer backend drives a passive PWM buzzer on the
``auxspace_buzzer`` chosen node. Every state transition first stops any
currently playing melody before issuing the new pattern.

.. list-table::
   :header-rows: 1
   :widths: 30 35 35

   * - Event / State
     - Pattern
     - Meaning
   * - Boot
     - 700 Hz tone for 150 ms (low blip)
     - System powered up.
   * - Calibration complete / Flight-ready
     - Rising chime 1400 => 2100 => 2800 Hz, 120 ms each
     - IMU calibration finished, rocket flight-ready. The ascending run
       is the positive "go" signal.
   * - Flight log written
     - 1400 Hz (120 ms) then 3500 Hz (280 ms): rising "ta-daa"
     - Flight record finalised on the filesystem.
   * - State => ``IDLE``
     - Falling two-tone 2100 => 1400 Hz, 150 ms each
     - Disarmed, safe. Descending run mirrors the ``ARMED`` beep.
   * - State => ``ARMED``
     - Two 2100 Hz beeps of 180 ms, separated by a 120 ms gap
     - Armed. **Pyros live.**
   * - State => ``APOGEE``
     - 2800 Hz tone for 500 ms (single long high tone)
     - Apogee detected, drogue event triggered.
   * - State => ``MAIN``
     - Two 2800 Hz beeps of 250 ms, separated by a 150 ms gap
     - Main parachute deployment event.
   * - State => ``REDUNDANT``
     - Three 3500 Hz beeps of 200 ms, separated by 150 ms gaps
     - Redundant (backup) parachute deployment event. One more beep,
       at a higher pitch, cleanly distinguishes it from ``MAIN``.
   * - State => ``LANDED``
     - "Astronomia" (Coffin Dance) melody, plays until interrupted
     - Flight complete. Acts as an audible recovery beacon.
   * - ``BOOST`` / ``BURNOUT``
     - 1400 Hz tone for 90 ms (neutral tick)
     - Nominal in-flight transition acknowledgement.
   * - Error
     - Five 4200 Hz beeps of 100 ms, separated by 100 ms gaps
     - Unrecoverable error. Service required.
   * - Powerfail
     - *(not handled)*
     - The buzzer backend does not implement ``on_powerfail``.

.. note::
   The ``LANDED`` melody is a looping recovery beacon and is the only
   pattern that runs asynchronously; it is stopped automatically on the
   next state transition.

Quick Reference
~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - State
     - LED
     - Buzzer
   * - ``IDLE``
     - Slow heartbeat (60 / 940 ms)
     - Falling 2100 => 1400 Hz · 150 ms
   * - ``ARMED``
     - Even blink (200 / 200 ms)
     - 2 · 2100 Hz · 180 ms beeps
   * - ``BOOST``
     - Off
     - 1400 Hz · 90 ms tick
   * - ``BURNOUT``
     - Off
     - 1400 Hz · 90 ms tick
   * - ``APOGEE``
     - Blink (500 / 500 ms)
     - 2800 Hz · 500 ms
   * - ``MAIN``
     - Blink (250 / 250 ms)
     - 2 · 2800 Hz · 250 ms beeps
   * - ``REDUNDANT``
     - Fast flicker (80 / 80 ms)
     - 3 · 3500 Hz · 200 ms beeps
   * - ``LANDED``
     - Slow beacon (700 / 300 ms)
     - "Astronomia" melody (looping)
   * - ``ERROR``
     - Solid ON
     - 5 · 4200 Hz · 100 ms beeps

Threading and Queueing
----------------------

The notification dispatcher (:c:func:`notify_state_change`,
:c:func:`notify_error`, ...) runs synchronously in the caller's
thread.  It fans out to each backend inline.  Individual backends may
choose to offload their work to avoid blocking flight-critical
threads.

**Buzzer backend** runs on a dedicated worker thread with a bounded
FIFO event queue:

- Calls from the state-machine task return immediately (they only
  enqueue an event), so blocking tone sequences (up to ~600 ms for
  the error pattern) never stall the 10 Hz state machine.
- Events are played in FIFO order. Important sequencing, notably
  stopping the ``LANDED`` melody before a new tone, is preserved
  because the worker thread runs ``pwm_melody_stop`` and the
  subsequent tone in one dequeued step.
- When the queue is full, new events are dropped with a
  ``LOG_WRN``. This gives natural back-pressure: the worker drains
  at the speed of its tone sequences, and bursty producers cannot
  unboundedly queue up noise.

Tunables (under ``AURORA_NOTIFY_BUZZER``):

.. list-table::
   :header-rows: 1
   :widths: 50 20 30

   * - Kconfig
     - Default
     - Purpose
   * - ``AURORA_NOTIFY_BUZZER_QUEUE_SIZE``
     - 16
     - Maximum queued events before overflow drops.
   * - ``AURORA_NOTIFY_BUZZER_STACK_SIZE``
     - 1024
     - Worker thread stack size (bytes).
   * - ``AURORA_NOTIFY_BUZZER_THREAD_PRIORITY``
     - 10
     - Worker thread priority. Keep numerically above flight
       threads (priority 5) so notifications never preempt them.

**LED backend** does not need a dedicated thread. Sustained state
patterns are continuous blinks handed to Zephyr's ``pwm-leds`` driver
(software timer), so ``on_state_change`` returns immediately and never
delays the state-machine task or pyro firing. The only inline sleeps are
the short one-shot flash bursts for discrete events: boot (≤ 500 ms,
before flight threads run), calibration complete (2 · 60 ms) and
flight-log written (3 · 80 ms, on the converter thread).  All outside
the flight hot path.

API Reference
-------------

.. doxygengroup:: lib_notify
   :content-only:
