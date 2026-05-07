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
     - IMU calibration has finished and the rocket is ready for arming.
   * - ``on_state_change``
     - Flight state-machine transition (see state table below).
   * - ``on_error``
     - An unrecoverable error condition was reported.
   * - ``on_powerfail``
     - A power failure was detected, or the system recovered from one.

.. _led-patterns:

LED Patterns
~~~~~~~~~~~~

The LED backend drives every LED child of the ``auxspace_led`` chosen
node in lockstep (same pattern on all LEDs). Brightness is 100%
(``MAX_BRIGHTNESS``) whenever the LED is lit.

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Event / State
     - Pattern
     - Meaning
   * - Boot
     - Solid ON for 500 ms, then OFF
     - System powered up and notification stack initialised.
   * - Calibration complete
     - Single 50 ms flash
     - IMU calibration finished, rocket ready to arm.
   * - State → ``IDLE``
     - Blink 50 ms ON / 450 ms OFF (short pulse, ~2 Hz)
     - Safe, disarmed. Awaiting arm command.
   * - State → ``ARMED``
     - Blink 200 ms ON / 200 ms OFF (even, ~2.5 Hz)
     - Armed and awaiting launch detection. **Pyros live.**
   * - State → ``LANDED``
     - Blink 400 ms ON / 100 ms OFF (long pulse, ~2 Hz)
     - Flight complete, rocket on ground. Safe to recover.
   * - State → ``ERROR``
     - Solid ON
     - Unrecoverable error. Service required.
   * - Any other state transition
     - All LEDs OFF
     - In-flight states (``BOOST``, ``BURNOUT``, ``APOGEE``, ``MAIN``,
       ``REDUNDANT``) are silent on the LED to save power and avoid
       optical noise during flight.
   * - Powerfail (loss)
     - All LEDs OFF; subsequent state/error events suppressed
     - Power dropped below threshold; backend is muted until recovery
       to conserve the backup rail.
   * - Powerfail (recover)
     - LEDs resume normal behaviour on the next event
     - Main power restored.

.. note::
   When a powerfail has been signalled and not yet recovered, the LED
   backend ignores state changes and error events. It will re-enable
   itself on the next event after ``on_powerfail(recover=1)``.

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
     - 4000 Hz tone for 500 ms
     - System powered up.
   * - Calibration complete
     - 1000 Hz tone for 500 ms
     - IMU calibration finished, rocket ready to arm.
   * - State → ``IDLE``
     - 500 Hz tone for 50 ms (low, short chirp)
     - Safe, disarmed.
   * - State → ``ARMED``
     - 2000 Hz tone for 200 ms (mid, clear beep)
     - Armed. **Pyros live.**
   * - State → ``APOGEE``
     - 3000 Hz tone for 300 ms (high, longer beep)
     - Apogee detected, drogue event triggered.
   * - State → ``MAIN``
     - 2500 Hz tone for 300 ms (mid-high, longer beep)
     - Main parachute deployment event.
   * - State → ``REDUNDANT``
     - Two 2500 Hz beeps of 150 ms, separated by a 100 ms gap
     - Redundant (backup) parachute deployment event. The double beep
       distinguishes the fallback path from the nominal ``MAIN`` event.
   * - State → ``LANDED``
     - "Astronomia" (Coffin Dance) melody, plays until interrupted
     - Flight complete. Acts as an audible recovery beacon.
   * - Any other state transition
     - Silent (any ongoing melody is stopped)
     - In-flight states ``BOOST`` and ``BURNOUT`` are silent on the
       buzzer.
   * - Error
     - Three 4000 Hz beeps of 100 ms, separated by 100 ms gaps
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
     - Short blink (50 / 450 ms)
     - 500 Hz · 50 ms
   * - ``ARMED``
     - Even blink (200 / 200 ms)
     - 2000 Hz · 200 ms
   * - ``BOOST``
     - Off
     - Silent
   * - ``BURNOUT``
     - Off
     - Silent
   * - ``APOGEE``
     - Off
     - 3000 Hz · 300 ms
   * - ``MAIN``
     - Off
     - 2500 Hz · 300 ms
   * - ``REDUNDANT``
     - Off
     - 2 × 2500 Hz · 150 ms beeps
   * - ``LANDED``
     - Long blink (400 / 100 ms)
     - "Astronomia" melody (looping)
   * - ``ERROR``
     - Solid ON
     - 3 × 4000 Hz · 100 ms beeps

Threading and Queueing
----------------------

The notification dispatcher (:c:func:`notify_state_change`,
:c:func:`notify_error`, ...) runs synchronously in the caller's
thread — it fans out to each backend inline. Individual backends may
choose to offload their work to avoid blocking flight-critical
threads.

**Buzzer backend** runs on a dedicated worker thread with a bounded
FIFO event queue:

- Calls from the state-machine task return immediately (they only
  enqueue an event), so blocking tone sequences (up to ~600 ms for
  the error pattern) never stall the 10 Hz state machine.
- Events are played in FIFO order. Important sequencing — notably
  stopping the ``LANDED`` melody before a new tone — is preserved
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

**LED backend** does not need a dedicated thread: blinking is
delegated to Zephyr's ``pwm-leds`` driver (software timer), and the
remaining inline sleeps are short (≤ 500 ms at boot, 50 ms on
calibration) and occur outside the flight hot path.

API Reference
-------------

.. doxygengroup:: lib_notify
   :content-only:
