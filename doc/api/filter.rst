Input Filtering
===============

Input filtering is an important part when dealing with noisy measurements.
Aurora currently only features one implementation, a simple predictive kalman
filter.
The filter tracks a 2-element state vector (altitude and vertical velocity) and
feeds its data to the state machine.

.. doxygengroup:: lib_filter
   :content-only:
