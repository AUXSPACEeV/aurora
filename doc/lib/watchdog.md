# Hardware Watchdog

A flight computer that stops running is worse than one that crashes.
A crash runs a fault handler, prints something and can reboot while a
wedged kernel does none of that.
The board simply goes quiet until someone power-cycles it, and on a
vehicle in the air nobody can.

This subsystem resets the board when the kernel stops running.
It is enabled with `CONFIG_AURORA_WATCHDOG` and pairs with
{doc}`flight state recovery <state>` so the reset resumes the flight
rather than rebooting into a disarmed vehicle.

## What it supervises

Kernel liveness, and deliberately nothing else.

A dedicated thread does nothing but feed a task-watchdog channel on a
timer.
If that thread stops being scheduled for
`CONFIG_AURORA_WATCHDOG_TIMEOUT_MS`, the board resets.
The feeder is preemptible on purpose: if a higher-priority
thread spins and starves everything below it, the board is not doing
useful work and resetting is the right outcome.

## What it does not supervise

It does **not** watch sensor data flow, and that is a design decision
rather than an omission.

An earlier attempt fed the watchdog from the state machine's zbus loop.
That loop waits on sensor messages, so a sensor that stopped producing
looked identical to a wedged kernel.
Worse, an I2C bus wedge on this hardware survives a warm reset, so the
board came back up with the same dead bus, failed to feed again and
reset in a loop.
It never recovered, and a reset loop mid-flight is strictly worse than
the fault it was trying to catch.

A sensor that goes quiet is a degraded flight the software should fly
through. See {doc}`sensors <sensors>` for how bus faults are handled.

## Why the hardware fallback is not optional

`CONFIG_AURORA_WATCHDOG` selects `CONFIG_TASK_WDT_HW_FALLBACK` and the
whole design rests on it.

Zephyr's task watchdog is driven by the kernel timer.
The failures this subsystem exists to catch include a stopped kernel
clock, and a supervisor that runs off the clock it is supervising stops
with it.
The hardware watchdog behind it (on ESP32 the TIMG MWDT) runs off its own
clock and fires regardless of what the CPU is doing.

This is the same reason `CONFIG_TASK_WDT` alone is not enough and why the
fallback cannot be turned off while keeping the guarantee.

## Board requirements

The board must expose a hardware watchdog through the `watchdog0` alias
and the node has to be enabled.
On many SoC devicetrees the watchdog nodes are `status = "disabled"` by
default, so an alias alone is not sufficient and the device will not be
ready at runtime:

```devicetree
/ {
   aliases {
      watchdog0 = &wdt0;
   };
};

&wdt0 {
   status = "okay";
};
```

A node left disabled is caught at
runtime: `aurora_watchdog_setup()` returns `-ENODEV` and logs that kernel
liveness is unsupervised.

## Configuration

| Symbol | Default | Meaning |
| :----- | :------ | :------ |
| `CONFIG_AURORA_WATCHDOG_TIMEOUT_MS` | 2000 | How long the feeder may go unscheduled before the board resets |
| `CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS` | 500 | How often the feeder feeds the channel |
| `CONFIG_AURORA_WATCHDOG_PRIORITY` | 10 | Feeder thread priority |
| `CONFIG_AURORA_WATCHDOG_STACK_SIZE` | 1024 | Feeder thread stack |

The timeout is to be kept comfortably above the feed interval.
Ordinary scheduling jitter, a logging burst or a flash write must not be
enough to trip it.
The default leaves room for three missed cycles.

```{note}
On Xtensa the feeder stack carries more than the function itself suggests.
Any interrupt taken while the thread runs pushes a base save area plus
spilled registers onto the *interrupted* thread's stack, so a thread can
be pushed over its limit by an interrupt it never asked for.
```

## Usage

```c
#include <aurora/lib/watchdog.h>

int main(void)
{
   /* Early, and before anything that can wedge. */
   (void)aurora_watchdog_setup();

   /* --snip-- */

   return 0;
}
```

Set it up early.
The failures it catches run no fault handler and print nothing, so any
window before it is armed is a window in which the board can go silently
dead.

## After a watchdog reset

The reset cause is readable through `hwinfo` and is reported as
`RESET_WATCHDOG` on the next boot.
The {doc}`state machine <state>` uses exactly that to decide whether to
resume the flight it was in, so the two subsystems are normally enabled
together.

:::{warning}
`CONFIG_TASK_WDT_HW_FALLBACK_PAUSE_HALTED_BY_DBG` defaults to `y`, which
pauses the hardware watchdog while a debugger has the CPU halted.
That is convenient for JTAG work, but it means watchdog behaviour cannot
be observed with a debugger attached.
Test it on a free-running board.
:::

## API Reference

```{doxygengroup} lib_watchdog
   :content-only:
```
