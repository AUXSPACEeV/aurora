State Machine
=============

The state machine library provides a generic interface for initializing,
updating, and querying the flight state.
As almost everything else in AURORA, it features a dynamic selection of state
machine types via Kconfig ``CONFIG_AURORA_STATE_MACHINE_TYPE``.
Currently only the simple state machine is implemented and it uses the
following flight sequence:

Simple State Machine
--------------------

The simple state machine
implementation defines a 9-state flight sequence driven by sensor thresholds.

.. image:: /img/aurora_simple_state_machine.drawio.svg
   :alt: states

.. list-table:: Signals
   :header-rows: 1
   :widths: auto

   * - Signal
     - Comment
   * - ARM
     - *ARM* signal from extern. Arms the pyro channels as well
   * - DISARM
     - *DISARM* signal from extern. Disarms the pyro channels as well

.. list-table:: Sensor Readings
   :header-rows: 1
   :widths: auto

   * - Sensor Reading
     - Comment
   * - T\ :sub:`AB`
     - Acceleration needed to go from ARMED to BOOST
   * - T\ :sub:`H`
     - Altitude needed to go from ARMED to BOOST
   * - T\ :sub:`BB`
     - Acceleration needed to go from BOOST to BURNOUT
   * - T\ :sub:`M`
     - Altitude needed to go from APOGEE to MAIN
   * - T\ :sub:`L`
     - Velocity needed to signal LANDED

.. list-table:: Timers
   :header-rows: 1
   :widths: auto

   * - Timer
     - Comment
   * - DT\ :sub:`AB`
     - Time that T_AB and T_H shall be asserted for
   * - DT\ :sub:`L`
     - Time that T_L shall be asserted

.. list-table:: Timeouts
   :header-rows: 1
   :widths: auto

   * - Timeout
     - Comment
   * - TO\ :sub:`A`
     - Timeout for APOGEE state
   * - TO\ :sub:`R`
     - Timeout for REDUNDANT state

State transitions are also driven by sensor thresholds configured via Kconfig
(boost acceleration, main descent height, apogee timeout, etc.).

API Reference
-------------

.. doxygengroup:: lib_state
   :content-only:
