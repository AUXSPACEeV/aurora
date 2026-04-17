Pyro Drivers
============

The pyrotechnic ignition driver class provides a hardware-agnostic API for
arming, triggering, and sensing pyro channels.
There is also a ``pyro-shell`` integration for interactive pyro testing.

Configuration
-------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - Option
     - Default
     - Description
   * - ``CONFIG_PYRO_INIT_PRIORITY``
     - 80
     - Kernel init priority for pyro driver instances.
   * - ``PYRO_SHELL``
     - n
     - Activate the shell integration for pyro drivers.

API-Reference
-------------

.. doxygengroup:: drivers_pyro
   :content-only:

Operations
----------

.. doxygengroup:: drivers_pyro_ops
   :content-only:
