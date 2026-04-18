Sensors
=======

The AURORA sensors library is merely an extension of the zephyr sensor driver
api with helper functions, different sampling methods and specific workflows.

IMU
---

Interface for the inertial measurement unit (e.g. LSM6DSO32). Provides
orientation (pitch and roll) and acceleration data.

.. doxygengroup:: lib_imu
   :content-only:

Attitude
--------

Gyro-integrated body-frame gravity tracker. Anchors the body-frame "up"
direction from a stationary accelerometer calibration window (typically
during ``SM_ARMED``), then propagates the gravity vector through flight
using gyro measurements to project body-frame acceleration onto the
world vertical axis for the filter.

Mounting orientation is configured via the ``CONFIG_IMU_UP_AXIS_*``
choice; calibration window length via ``CONFIG_IMU_CALIBRATION_SAMPLES``.

.. doxygengroup:: lib_attitude
   :content-only:

Barometer
---------

Interface for the barometric pressure sensor. Can compute altitude estimates
from pressure readings.

.. doxygengroup:: lib_baro
   :content-only:
