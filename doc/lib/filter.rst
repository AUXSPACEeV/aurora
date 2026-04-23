Input Filtering
===============

AURORA is built to use different filter techniques for apogee detection,
controlled by ``CONFIG_FILTER_TYPE``.

Kalman Filter
-------------

One implementation uses a 2-state Kalman filter with vertical
acceleration as a control input
(guarded by ``CONFIG_FILTER`` and ``CONFIG_FILTER_KALMAN``).
The filter tracks a two-element state vector, consisting of altitude and
vertical velocity, and is fed barometric altitude measurements that the baro
library converts from pressure via the ISA hypsometric formula.

On every state-machine tick the filter runs a predict/update cycle:

- The predict step propagates altitude and velocity forward using the
  world-frame vertical acceleration as a control input
  (:math:`B = [\tfrac{1}{2} \Delta t^2,\; \Delta t]^\top`) while
  growing the covariance by a process-noise term scaled with
  :math:`\Delta t`. :math:`\Delta t` is clamped to 1 s to prevent
  filter blow-up.
- The update step corrects the state with the latest barometric
  altitude reading through the standard Kalman gain. A
  normalised-innovation-squared (NIS / Mahalanobis) gate of 25
  (:math:`\approx 5\sigma`) rejects obvious sensor glitches; the gate
  is disabled for the first 30 updates (~3 s at 10 Hz) to ride out the
  initial transient before the prior covariance settles.

Apogee detection combines three criteria evaluated on every call to
``filter_detect_apogee()``:

- velocity estimate has gone non-positive,
- the running peak altitude is no longer being beaten (descent), and
- the last applied vertical acceleration sits inside a
  :math:`\pm 20\,\mathrm{m/s^2}` coast band, which rejects boost-phase
  kinematics.

All three must hold for ``CONFIG_FILTER_APOGEE_DEBOUNCE_SAMPLES``
consecutive samples before apogee is latched and the
:math:`\text{BURNOUT} \rightarrow \text{APOGEE}` state transition is
reported. Once latched, the filter keeps reporting
apogee without re-evaluating the criteria.

Three tunable noise parameters (altitude process noise Q_alt, velocity
process noise Q_vel, and measurement noise R) plus the debounce sample
count are exposed as Kconfig options so they can be adjusted without
recompiling the filter source.

A Python simulation tool
(`tools/sim_flight_kalman.py <https://github.com/AUXSPACEeV/aurora/blob/main/tools/sim_flight_kalman.py>`_)
reproduces the same algorithm with a realistic flight profile and MS5607
sensor-noise model, allowing the filter to be tuned and validated offline before
flight. See :doc:`/tools/sim_flight_kalman` for usage and example output.

The filter and its hypsometric pipeline are covered by a ztest suite under
`aurora/tests/lib/filter/ <https://github.com/AUXSPACEeV/aurora/tree/main/tests/lib/filter>`_.

Configuration
~~~~~~~~~~~~~

Noise parameters are integer-scaled by 1000 in Kconfig.
The actual float value is ``CONFIG_FILTER_*_MILLISCALE / 1000.0``.

.. list-table::
   :header-rows: 1
   :widths: auto

   * - Option
     - Default
     - Description
   * - ``CONFIG_FILTER_Q_ALT_MILLISCALE``
     - 100
     - Process noise for altitude state (Q_alt).
       Increase if altitude estimate lags behind reality.
   * - ``CONFIG_FILTER_Q_VEL_MILLISCALE``
     - 500
     - Process noise for velocity state (Q_vel).
       Increase for more aggressive response during boost.
   * - ``CONFIG_FILTER_R_MILLISCALE``
     - 4000
     - Measurement noise variance (R). Higher values trust the barometer less.
   * - ``CONFIG_FILTER_APOGEE_DEBOUNCE_SAMPLES``
     - 3
     - Consecutive ``filter_detect_apogee()`` calls for which all apogee
       criteria (:math:`v \leq 0`, no new peak, coast-band acceleration)
       must hold before apogee is latched. Not milliscaled.

API-Reference
-------------

.. doxygengroup:: lib_filter
   :content-only:
