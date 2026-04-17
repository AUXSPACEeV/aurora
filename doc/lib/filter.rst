Input Filtering
===============

AURORA is built to use different filter techniques for apogee detection,
controlled by ``CONFIG_FILTER_TYPE``.

Kalman Filter
-------------

One implementation uses a scalar constant-velocity Kalman filter
(guarded by ``CONFIG_FILTER`` and ``CONFIG_FILTER_KALMAN``).
The filter tracks a two-element state vector — altitude and vertical
velocity — and is fed barometric altitude measurements that the baro
library converts from pressure via the ISA hypsometric formula.

On every state-machine tick the filter runs a predict/update cycle:

- The predict step propagates altitude forward using the current velocity
  estimate while growing the covariance by a process-noise term scaled
  with dt
- The update step corrects the state with the latest barometric
  altitude reading through the standard Kalman gain.

Apogee is declared the moment the estimated velocity crosses zero from
positive to non-positive, which in turn triggers the BURNOUT → APOGEE state
transition.
Three tunable noise parameters (altitude process noise Q_alt, velocity
process noise Q_vel, and measurement noise R) are exposed as
milliscale Kconfig options so they can be adjusted without recompiling
the filter source.

A Python simulation tool
(`tools/sim_flight_kalman.py <https://github.com/AUXSPACEeV/aurora/blob/main/tools/sim_flight_kalman.py>`_)
reproduces the same algorithm with a realistic flight profile and MS5607
sensor-noise model, allowing the filter to be tuned and validated offline before
flight. On run, it produces a graph and saves it to a file called
``flight_simulation.png``. Here is an example run:

.. code-block:: bash

   Plot saved to flight_simulation.png
   Ground ref pressure: 101340 Pa (1013.4 hPa)
   True apogee:         500.0 m
   Filter apogee:       496.4 m (t = 12.56 s)

.. only:: not dark

   .. image:: /img/flight_simulation.png

.. only:: dark

   .. image:: /img/flight_simulation_dark.png
      :alt: flight_simulation.png

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

API-Reference
-------------

.. doxygengroup:: lib_filter
   :content-only:
