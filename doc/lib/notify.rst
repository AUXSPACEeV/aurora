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
- **PWM LED** (``CONFIG_AURORA_NOTIFY_LED``): drives an LED via PWM
  to signal boot, state changes, and errors. LED does not blink when data logger
  is disabled.

API Reference
-------------

.. doxygengroup:: lib_notify
   :content-only:
