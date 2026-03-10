# Introduction

This is the API documentation for [AURORA], the **AU**xspace **RO**cket ope**RA**ting System.

AURORA is a Zephyr RTOS-based avionics firmware for the METER-2 rocket,
developed by [Auxspace e.V.](https://auxspace.de). It manages sensor data
(IMU, barometer), flight state detection, and pyrotechnic ignition across
multiple interconnected boards communicating over CAN bus.

## Modules

- @ref drivers — Pyro ignition driver class and implementations
- @ref lib — Sensor, state machine, and apogee detection libraries

[AURORA]: https://github.com/AUXSPACEeV/aurora
