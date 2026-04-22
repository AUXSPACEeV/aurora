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

Shell Commands
--------------

Enabling ``CONFIG_PYRO_SHELL`` registers the ``pyro`` command group for
interactive bring-up, bench testing and ground-checkouts of pyro channels.
All channel commands take a devicetree device name and a numeric channel
index.

.. list-table::
   :header-rows: 1
   :widths: auto

   * - Command
     - Description
   * - ``pyro devices``
     - List all ready pyro devices and their channel counts.
   * - ``pyro channels <device>``
     - List the channel indices of ``<device>``.
   * - ``pyro state <device>``
     - For every channel, print capacitor voltage and sense voltage (in mV).
   * - ``pyro arm <device> <channel>``
     - Arm a pyro channel.
   * - ``pyro disarm <device> <channel>``
     - Disarm a pyro channel.
   * - ``pyro trigger <device> <channel>``
     - Fire a pyro channel. The channel must be armed.
   * - ``pyro secure <device> <channel>``
     - Secure (short) a pyro channel to safely dissipate stored charge.
   * - ``pyro sense <device> <channel>``
     - Read the sense-ADC of a channel (mV).
   * - ``pyro cap <device> <channel>``
     - Read the firing-capacitor voltage of a channel (mV).

.. warning::
   ``pyro trigger`` will fire a live pyrotechnic channel. Only use this
   command with igniters disconnected or in an explicitly safe test
   configuration.

API-Reference
-------------

.. doxygengroup:: drivers_pyro
   :content-only:

Operations
----------

.. doxygengroup:: drivers_pyro_ops
   :content-only:
