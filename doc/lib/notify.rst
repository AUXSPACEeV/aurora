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
  buzzer via PWM to signal boot, state changes, and errors.
  See `Buzzer Patterns`_.
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
     - Apogee detected, drogue / main event triggered.
   * - State → ``LANDED``
     - "Astronomia" (Coffin Dance) melody, plays until interrupted
     - Flight complete. Acts as an audible recovery beacon.
   * - Any other state transition
     - Silent (any ongoing melody is stopped)
     - In-flight states other than ``APOGEE`` are silent on the buzzer.
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
     - Silent
   * - ``REDUNDANT``
     - Off
     - Silent
   * - ``LANDED``
     - Long blink (400 / 100 ms)
     - "Astronomia" melody (looping)
   * - ``ERROR``
     - Solid ON
     - 3 × 4000 Hz · 100 ms beeps

API Reference
-------------

.. doxygengroup:: lib_notify
   :content-only:
