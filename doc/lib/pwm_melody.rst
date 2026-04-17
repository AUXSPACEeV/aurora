PWM Melodies
============

A landed rocket needs a way to communicate its position to the searching squad.
One simple way of making it easier to find is to play loud buzzer sounds in an
endless loop until the recovery team finds it.

But only playing loud noises is boring and annoying in testing.
Instead, use the PWM Melody API to play music of your liking:

.. code-block:: devicetree

   / {
      buzzer0: buzzer_0 {
         compatible = "auxspaceev,pwm-buzzer";
         pwms = <&ledc0 0 PWM_MSEC(200) PWM_POLARITY_NORMAL>;
      };
   };

.. code-block:: c

   #include <aurora/lib/pwm_melody.h>

   // dt node that contains a "pwms" child node
   static const struct pwm_dt_spec buzzer =
      PWM_DT_SPEC_GET(DT_NODELABEL(buzzer0));

   // play astronomia from aurora/lib/pwm_melody.h
   PWM_MELODY_CTX_DEFINE(melody_ctx, &buzzer, astronomia, 1024);

   int main()
   {
      pwm_melody_start(&melody_ctx);

      // --snip--
      // play as long as needed
      // --snip

      pwm_melody_stop(&melody_ctx);
   }

API Reference
-------------

.. doxygengroup:: lib_pwm_melody
   :content-only:
