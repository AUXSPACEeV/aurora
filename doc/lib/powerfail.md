# Powerfail Mitigation

Flight computers often log data on storage devices, like µSD-Cards, eMMCs or
other NAND-flashes.
To protect against broken file systems (or even blocks) on powerloss, we have
a small powerfail mitigation subsystem to handle power failures in a way that
preserves data.

When a power failure is detected the subsystem stops every registered data
logger, so the card is not left mid-write, raises the powerfail
{doc}`notification <notify>`, invokes the application's callback and, with
`CONFIG_AURORA_POWERFAIL_SHUTDOWN`, powers the board down. When power comes
back, the loggers are released again and the deassert callback runs.

A power failure event is detected by a configured gpio pin.
In a devicetree, the setup for a pulled-up GPIO pin would look like this:

```devicetree
/ {
   chosen {
      auxspace,pfm = &pfm_in;
   };

   buttons {
      compatible = "gpio-keys";

      pfm_in: pfm_in {
         gpios = <&gpio0 0 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
         label = "Powerfail Pin";
      };
   };
}
```

The line is sampled once during {c:func}`powerfail_setup`, so a board that
boots with power already failing does not wait for an edge that has been and
gone. Callbacks run in **interrupt context**. Keep them short, power may
be milliseconds away from collapsing.

## Why a GPIO and not the battery voltage

The board has to provide that line, from a supply supervisor's power-fail
output or a comparator on the bulk capacitor. Measuring the pack through a
battery-sense divider is *not* an alternative, for two reasons:

- **An ADC cannot interrupt on a threshold.** Zephyr's ADC API has no window,
  watchdog or limit concept at all. It starts a conversion sequence and hands
  back samples. Some SOCs can do it (the ESP32-S3 has two digital ADC
  monitors), but it is a continuous/DMA-mode feature that Zephyr neither
  exposes nor compiles in. Zephyr's generic comparator API (`comparator.h`)
  is the portable route, on SoCs that have an analog comparator to drive it.
- **Polling is too late for the case that matters.** A pack running down
  crosses any threshold over minutes, which the vbat telemetry already shows.
  A connector letting go is instant, and no sampling rate catches it. Power
  is gone before the next conversion. What buys time there is bulk
  capacitance plus a supervisor that fires while the cap still holds charge.

Boards without such a line log the battery voltage as telemetry (see the
{doc}`flight computer <../applications/flight_computer>` hardware requirements) and
leave this subsystem disabled.

## Usage

```c
#include <aurora/lib/powerfail.h>

static void powerfail_assert(void)
{
   // Gets invoked when power starts failing, after the loggers were stopped
}

static void powerfail_deassert(void)
{
   // Gets invoked when power is restored
}

int main(void)
{
   powerfail_setup(&powerfail_assert, &powerfail_deassert);

   // --snip--

   return 0;
}
```

Both callbacks are optional; pass `NULL` if the built-in mitigation is all
you need.

```{note}
Earlier revisions of AURORA pointed this subsystem at the remove-before-flight
pin and used "power failing" to mean "disarmed". That job now belongs to the
{doc}`state machine <state>` (`CONFIG_AURORA_STATE_MACHINE_RBF`); powerfail is
about power again.
```

## API Reference

```{doxygengroup} lib_powerfail
   :content-only:
```
