Powerfail Mitigation
====================

Flight computers often log data on storage devices, like µSD-Cards, eMMCs or
other NAND-flashes.
To protect against broken file systems (or even blocks) on powerloss, we have
a small powerfail mitigation subsystem to handle power failures in a way that
preserves data.

A power failure event is detected by a configured gpio pin.
In a devicetree, the setup for a pulled-up GPIO pin would look like this:

.. code-block:: devicetree

   / {
      chosen {
         auxspace,pfm = &button0;
      };

      buttons {
         compatible = "gpio-keys";

         button0: button_0 {
            gpios = <&gpio0 0 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
            label = "Powerfail Pin";
         };
      };
   }

The subsystem is named Powerfail Mitigation, but saving and recovering state
can also be of use outside of the powerloss scope.
Another example of using the Powerfail Signal is to stop data loggers, when
the avionic system is in a disarmed state.
In this case, the pin can act as an arm signal and start the data logger, when
the pin is pulled and vice-versa:

.. code-block:: c

   #include <aurora/lib/powerfail.h>

   static int armed = 0;

   static void powerfail_assert()
   {
      // Gets invoked, when powerfail pin is asserted
      armed = 0;
   }

   static void powerfail_deassert()
   {
      // Gets invoked, when powerfail pin is deasserted
      armed = 1;
   }

   int main(void)
   {
      powerfail_setup(&powerfail_assert, &powerfail_deassert);

      // --snip--
      // use "armed" here e.g. in a loop
      // --snip--

      return 0;
   }

API Reference
-------------

.. doxygengroup:: lib_powerfail
   :content-only:
