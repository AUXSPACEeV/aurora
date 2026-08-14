# State Machine

The state machine library provides a generic interface for initializing,
updating, and querying the flight state.
As almost everything else in AURORA, it features a dynamic selection of state
machine types via Kconfig `CONFIG_AURORA_STATE_MACHINE_TYPE`.
Currently only the simple state machine is implemented and it uses the
following flight sequence:

## Simple State Machine

The simple state machine
implementation defines a 9-state flight sequence driven by sensor thresholds.

(simple-state-machine-diagram)=

```{image} /img/aurora_simple_state_machine.drawio.svg
:alt: states
```

```{table} Signals
| Signal | Comment |
| --- | --- |
| ARM | *ARM* signal from extern. Arms the pyro channels as well |
| DISARM | *DISARM* signal from extern. Disarms the pyro channels as well |
```

```{table} Sensor Readings
| Sensor Reading | Comment |
| --- | --- |
| T<sub>AB</sub> | Acceleration needed to go from ARMED to BOOST |
| T<sub>H</sub> | Altitude needed to go from ARMED to BOOST |
| T<sub>BB</sub> | Acceleration needed to go from BOOST to BURNOUT |
| T<sub>M</sub> | Altitude needed to go from APOGEE to MAIN |
| T<sub>L</sub> | Velocity needed to signal LANDED |
```

```{table} Timers
| Timer | Comment |
| --- | --- |
| DT<sub>AB</sub> | Time that T_AB and T_H shall be asserted for |
| DT<sub>L</sub> | Time that T_L shall be asserted |
```

```{table} Timeouts
| Timeout | Comment |
| --- | --- |
| TO<sub>A</sub> | Timeout for APOGEE state |
| TO<sub>R</sub> | Timeout for REDUNDANT state |
```

State transitions are also driven by sensor thresholds configured via Kconfig
(boost acceleration, main descent height, apogee timeout, etc.).

(rbf)=
## Remove Before Flight

The ARM/DISARM signal normally comes from the application, through
`sm_inputs.armed`. Setting `CONFIG_AURORA_STATE_MACHINE_RBF` takes it from
hardware instead: the state machine reads the mechanical safety lock (the key
or shorting plug that is pulled off the rocket on the pad) and substitutes the
pin for `armed` on every update. Whatever the application writes into that
field is ignored, so software arming and the physical "is the streamer still
in?" check can never disagree.

The GPIO is selected by the `auxspace,rbf` chosen node and follows the usual
"safe when made" convention:

```devicetree
/ {
   chosen {
      auxspace,rbf = &rbf_in;
   };

   buttons {
      compatible = "gpio-keys";

      rbf_in: rbf_in {
         gpios = <&gpio1 6 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
         label = "RBF Button";
      };
   };
}
```

| "Remove Before Flight"-Plug | Line | Vehicle |
| --------------------------- | ---- | ------- |
| installed | asserted | `SAFE`: machine held in `IDLE` |
| removed | deasserted | `ARMED`: machine free to leave `IDLE` |

Every edge restarts a debounce window
(`CONFIG_AURORA_STATE_MACHINE_RBF_DEBOUNCE_MS`, default 50 ms) and the level is
only sampled once the contact has been quiet for a full window, so a chattering
plug cannot arm and disarm the machine repeatedly. Changes are recorded in the
audit log.

The level is also **not** latched during `sm_init()`, before the interrupt is
enabled: a board that boots with the interlock already pulled never produces
an edge and will sit disarmed forever waiting for one. If the pin
cannot be brought up at all the interlock reports safe, holding the machine in
`IDLE` rather than arming on an input it cannot read.

```{note}
This used to be done by pointing the {doc}`powerfail <powerfail>` subsystem at
the RBF pin and treating "power failing" as "disarmed". That is no longer the
case: powerfail watches the battery, the state machine watches the interlock,
and the two are independent.
```

(state-thresholds)=
## Flight Thresholds

The thresholds that drive the transitions are per-vehicle data, not a
firmware constant: a 200 m model and a 1 km vehicle disagree on every
altitude and timeout in the set. The Kconfig options
(see {ref}`the sensor_board tables <configuration>`) are the factory
defaults; the running values are edited from the shell and persisted, so a
board flashed with one firmware image can fly either rocket.

The store is a single flash erase page selected by the `auxspace,sm-config`
chosen node, holding one record: a magic, a layout version, a CRC and the
threshold struct. Every save erases and rewrites the whole page. A blank
page, a firmware update that reshuffled `struct sm_thresholds`, or a write
cut short by a power loss all fail their check and fall back to the
defaults, so a half-written set is never flown.

Reserve the page in the board devicetree, e.g. for micrometer rev.2:

```dts
/ {
	chosen {
		auxspace,sm-config = &sm_config_partition;
	};
};

&flash0 {
	partitions {
		sm_config_partition: partition@3df000 {
			compatible = "zephyr,mapped-partition";
			label = "sm-config";
			reg = <0x3df000 DT_SIZE_K(4)>;
		};
	};
};
```

Without the chosen node (or with `CONFIG_AURORA_STATE_MACHINE_CONFIG_STORE`
disabled) the thresholds still change at runtime, but the shell warns that
the change is lost on reboot.

Thresholds can only be changed in `IDLE`. Swapping one mid-flight would
compare fresh limits against timers already started under the old ones, so
`sm_set_thresholds()` returns `-EBUSY` outside `IDLE`.

## Flight State Recovery

With `CONFIG_AURORA_STATE_MACHINE_RETAIN`, the active flight state survives a
watchdog reset. Without it, a board that reboots mid-flight comes back in
`IDLE` — a silently disarmed vehicle, with the recovery charges no longer
going to fire.

The record lives in RTC slow memory (`.rtc_noinit`), which the linker marks
NOLOAD and the startup code does not clear, so it survives the warm reset a
{doc}`watchdog <watchdog>` produces. It is written on every transition
through `sm_transition()`, which is the only sanctioned way to change state
and therefore the one place the record cannot drift out of sync.

### Why not flash

Flash would also survive losing power, but the state machine writes on
*every* transition, and the transitions that matter most (`APOGEE`, `MAIN`,
`REDUNDANT`) are exactly the ones that must not stall. A page erase blocks
for milliseconds with interrupts locked and contends with the flight
recorder already writing that device. The write that matters is the one
issued while the system is already misbehaving, which is the worst possible
moment to start erasing pages.

RTC memory costs a few stores, has no wear and needs no erase. The trade-off
is that it does **not** survive a power cycle or a brownout — see
{doc}`powerfail <powerfail>` for that failure, which needs a different
mitigation.

### When recovery happens

All of the following must hold, or the machine starts in `IDLE`:

- the reset cause latched at boot includes `RESET_WATCHDOG`,
- the record carries the expected magic, version and payload size,
- the CRC matches,
- it was written by the same state-machine backend (`sm_type`), so swapping
  implementations cannot resurrect a state ID that now means something else,
- the retained state is not `IDLE`, and
- the {ref}`RBF interlock <rbf>` reads *removed* when re-sampled.

A power-up, a reset button or a debugger attach therefore always lands in
`IDLE`. That is deliberate: the operator needs a reliable way to reach a
known-safe state, and holding reset must not resurrect a live flight.

### The interlock has to be re-sampled

`sm_rbf_init()` leaves the vehicle reported *safe* at boot and waits for an
edge, so that forgetting to install the plug cannot arm the machine. That is
right for a cold boot and wrong here: in flight the plug was pulled long
before the reset, no further edge is coming, and the machine would disarm
itself on its first update — undoing the recovery a few milliseconds after
making it.

The recovery path therefore calls `sm_rbf_resync()` to adopt the pin's
present level, but only once it knows the reset came from the watchdog and a
valid in-flight record exists. Under those conditions the vehicle was
demonstrably armed already, which makes the pin the authority rather than a
guess. A plug that is still installed still reads safe, so resetting on the
pad still disarms.

### Attitude calibration comes back too

Calibration only runs while the machine is in `IDLE` (see the
{doc}`sensor board <../applications/sensor_board>` IMU handling), so a board
that resumes into a flight state can never redo it. Losing it is not
cosmetic: vertical acceleration would stay pinned at zero, leaving boost and
apogee detection blind, and orientation would integrate from a zero reference
— reading horizontal, which trips the elevation gate and disarms within
`N_OI` samples.

The calibration is therefore snapshotted into the same record the moment it
completes, while the vehicle is stationary and the result is known good, and
handed back on recovery. The biases were measured on the pad and do not
change across a reset, and there is no opportunity to measure them again
mid-flight.

If the reset landed before calibration ever finished there is nothing to
restore. The flight continues, but degraded, and says so in the log.

### Flight timers

Timers restart from zero. There is no way to know how long the board was
absent, so phase timeouts (`TO_A`, `TO_R`) are necessarily generous after a
recovery rather than wrong in the unsafe direction.

### Reading the audit log

A recovery leaves a recognisable trace:

```
transition   IDLE   ARMED
event        ARMED  recovered after watchdog reset
```

A refusal leaves `recovery refused: interlock safe` instead. If you see the
resume followed by an `ARMED -> IDLE` transition with **no event between
them**, that is the disarm check in the backend, not the orientation gate —
the orientation path logs `orientation below threshold` and the log-offline
path logs `arm aborted: flight log offline`, so an eventless transition
means `.armed` went false.

## Shell Commands

Enabling `CONFIG_AURORA_STATE_MACHINE_SHELL` registers the
`state_machine` command group. Audit-log commands are only available when
`CONFIG_AURORA_STATE_MACHINE_AUDIT` is also enabled.

| Command | Description |
| --- | --- |
| `state_machine status` | Print the active state-machine implementation and its current state. |
| `state_machine transition <STATE>` | Force a transition. The state name completes via tab. Because the state machine exposes no arbitrary setter, this deinitializes and reinitializes the machine, landing it in `IDLE`; a warning is printed when the requested target is not `IDLE`. Ground testing only. |
| `state_machine config` | List the running thresholds next to the compiled-in defaults. |
| `state_machine config set <NAME> <VALUE>` | Set one threshold, apply it and save the whole set. The name completes via tab and is case-insensitive; out-of-range values are rejected. |
| `state_machine config default` | Restore the factory (Kconfig) thresholds and save them. |
| `state_machine config save` | Save the running thresholds as they are. |
| `state_machine audit` | Dump the audit log (timestamped transitions and events). Requires `CONFIG_AURORA_STATE_MACHINE_AUDIT`. |
| `state_machine audit_clear` | Clear the audit log. Requires `CONFIG_AURORA_STATE_MACHINE_AUDIT`. |

Setting the main deployment altitude to 400 m and keeping it:

```
uart:~$ state_machine config set T_M 400
T_M = 400 m
Applied and saved
```

Valid state names for `transition` are `IDLE`, `ARMED`, `BOOST`,
`BURNOUT`, `APOGEE`, `MAIN`, `REDUNDANT`, `LANDED` and `ERROR`.

:::{warning}
`state_machine transition` bypasses normal flight logic and resets the
machine. Do not use in flight.
:::

## API Reference

```{doxygengroup} lib_state
   :content-only:
```

```{doxygengroup} lib_state_config
   :content-only:
```

```{doxygengroup} lib_state_retain
   :content-only:
```
