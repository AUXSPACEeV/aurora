# Disk activity LED

A blinking "the storage card is busy" light. The same idea as the little
activity LED on a USB stick or an old hard drive. It flickers while data is
being written to the card and stays dark when the card is idle.

During a flight the board streams sensor data to the SD card. This module
drives a small GPIO LED so a human standing next to the rocket can *see* that
logging is actually happening.

It is kept completely separate from the PWM status LED used by the rest of the
{doc}`notification library <notify>`: that LED shows the **flight state**, this
one shows **disk activity**, and they sit on different pins so they can never
overwrite each other.

## How it works

Whatever code writes to the SD card (the flight data logger) calls
{c:func}`disk_led_activity` once per write. Each call does two things:

1. turns the LED **on**, and
2. arms a countdown (the "hold timer") to turn it **off** again a few
   milliseconds later (`CONFIG_AURORA_NOTIFY_DISK_LED_HOLD_MS`, default 40 ms).

The trick is that every new write *restarts* the countdown. So the timer only
ever fires once the card has been quiet for a whole hold window:

| What the card is doing | What you see on the LED |
| ---------------------- | ----------------------- |
| a single quick write   | one short blink         |
| a rapid burst of writes| a fast flicker          |
| continuous writing     | looks steadily on       |
| idle                   | off                     |

The "turn off later" step runs on a Zephyr *delayable work item* rather than by
sleeping, so {c:func}`disk_led_activity` returns immediately and never stalls
the logger thread that called it.

## How to wire it into a board

A board opts in with two things:

1. a devicetree `chosen` entry `auxspace,disk-led` pointing at a `gpio-leds`
   child node that says which pin the LED is on, for example:

   ```dts
   / {
       status_leds: status-leds {
           compatible = "gpio-leds";

           sd_activity_led: led_sd {
               gpios = <&gpio1 7 GPIO_ACTIVE_HIGH>;
               label = "SD Activity LED";
           };
       };

       chosen {
           auxspace,disk-led = &sd_activity_led;
       };
   };
   ```

2. `CONFIG_AURORA_NOTIFY_DISK_LED=y` in the board's application config.

If either is missing, {c:func}`disk_led_activity` compiles down to a
do-nothing call, so shared code (such as the data logger) can invoke it
unconditionally without `#ifdef`s.

## Configuration

| Kconfig | Default | Purpose |
| ------- | ------- | ------- |
| `CONFIG_AURORA_NOTIFY_DISK_LED` | n | Enable the SD activity LED backend. |
| `CONFIG_AURORA_NOTIFY_DISK_LED_HOLD_MS` | 40 | How long the LED stays lit after the most recent disk access. |

## API Reference

```{doxygengroup} lib_disk_led
:content-only:
```
