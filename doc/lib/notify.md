# Notifications

The notification library provides an abstract interface for user-facing
indicators (buzzer, RGB LED, ...).
Each backend registers a {c:struct}`notify_backend` at link time via an iterable
section.
The library fans out every call to all enabled backends.

## Backends

- **PWM Buzzer** (`CONFIG_AURORA_NOTIFY_BUZZER`): drives a passive
  buzzer via PWM to signal boot, calibration start/completion, state
  changes, and errors. Runs on
  a dedicated worker thread so that the blocking tone sequences do
  not stall the caller (typically the state-machine task).
  See [Buzzer Patterns] and [Threading and Queueing].
- **PWM LED** (`CONFIG_AURORA_NOTIFY_LED`): drives an LED via PWM
  to signal boot, state changes, and errors. LED does not blink when data logger
  is disabled. See [LED Patterns].

## Notification Patterns

The following tables describe exactly what each backend does for every
event in the notification API. Patterns are kept short and distinctive so
operators on the launch pad can identify system state by ear and eye alone.

### Events

These are the events (hooks) dispatched by the notification library.
Every registered backend reacts independently; not every backend reacts
to every event.

| Event | When it fires |
| --- | --- |
| `on_boot` | Once at system startup, after backends are initialised. |
| `on_calibration_start` | IMU calibration has begun accumulating a stationary window. Raised once per calibration cycle, not on the internal restarts the attitude tracker performs whenever it detects motion. |
| `on_calibration_complete` | IMU calibration has finished and the rocket is ready for arming. |
| `on_state_change` | Flight state-machine transition (see state table below). |
| `on_error` | An unrecoverable error condition was reported. |
| `on_powerfail` | A power failure was detected, or the system recovered from one. |

(led-patterns)=

### LED Patterns

The LED backend drives every LED child of the `auxspace_led` chosen
node in lockstep (same pattern on all LEDs). Brightness is 100%
(`MAX_BRIGHTNESS`) whenever the LED is lit.

| Event / State | Pattern | Meaning |
| --- | --- | --- |
| Boot | Solid ON for 500 ms, then OFF | System powered up and notification stack initialised. |
| Calibration started | *(not handled)* | The LED backend does not implement `on_calibration_start`; the `IDLE` blink already covers this phase. Use the buzzer cue. |
| Calibration complete | Single 50 ms flash | IMU calibration finished, rocket ready to arm. |
| State → `IDLE` | Blink 50 ms ON / 450 ms OFF (short pulse, ~2 Hz) | Safe, disarmed. IMU bias calibration runs in the background here; pyros are **not** yet live. |
| State → `ARMED` | Blink 200 ms ON / 200 ms OFF (even, ~2.5 Hz) | Calibrated and awaiting launch detection. **Pyros live.** |
| State → `LANDED` | Blink 400 ms ON / 100 ms OFF (long pulse, ~2 Hz) | Flight complete, rocket on ground. Safe to recover. |
| State → `ERROR` | Solid ON | Unrecoverable error. Service required. |
| Any other state transition | All LEDs OFF | In-flight states (`BOOST`, `BURNOUT`, `APOGEE`, `MAIN`, `REDUNDANT`) are silent on the LED to save power and avoid optical noise during flight. |
| Powerfail (loss) | All LEDs OFF; subsequent state/error events suppressed | Power dropped below threshold; backend is muted until recovery to conserve the backup rail. |
| Powerfail (recover) | LEDs resume normal behaviour on the next event | Main power restored. |

:::{note}
When a powerfail has been signalled and not yet recovered, the LED
backend ignores state changes and error events. It will re-enable
itself on the next event after `on_powerfail(recover=1)`.
:::

(buzzer-patterns)=

### Buzzer Patterns

The buzzer backend drives a passive PWM buzzer on the
`auxspace_buzzer` chosen node. Every state transition first stops any
currently playing melody before issuing the new pattern.

| Event / State | Pattern | Meaning |
| --- | --- | --- |
| Boot | 4000 Hz tone for 500 ms | System powered up. |
| Calibration started | Two 1000 Hz beeps of 100 ms, separated by a 100 ms gap | The stationary IMU calibration window has begun accumulating (during `IDLE`). Same pitch as the "Calibration complete" tone below so both read as "calibration" by ear, with the rhythm distinguishing them (two short = started, one long = done); deliberately low against the 4000 Hz boot jingle that precedes it in the normal power-on flow. Fires once per calibration cycle — the window restarts the attitude tracker performs on motion are silent. |
| Calibration complete | 1000 Hz tone for 500 ms | IMU calibration finished (runs in the background during `IDLE`), rocket ready to arm. When arming is requested and calibration is already done, this is immediately followed by the `ARMED` beep below. |
| State → `IDLE` | 500 Hz tone for 50 ms (low, short chirp) | Safe, disarmed. |
| State → `ARMED` | 2000 Hz tone for 200 ms (mid, clear beep — higher/longer than the "Calibration complete" tone above so the two are distinguishable by ear) | Calibrated. **Pyros live.** |
| State → `APOGEE` | 3000 Hz tone for 300 ms (high, longer beep) | Apogee detected, drogue event triggered. |
| State → `MAIN` | 2500 Hz tone for 300 ms (mid-high, longer beep) | Main parachute deployment event. |
| State → `REDUNDANT` | Two 2500 Hz beeps of 150 ms, separated by a 100 ms gap | Redundant (backup) parachute deployment event. The double beep distinguishes the fallback path from the nominal `MAIN` event. |
| State → `LANDED` | "Astronomia" (Coffin Dance) melody, plays until interrupted | Flight complete. Acts as an audible recovery beacon. |
| Any other state transition | Silent (any ongoing melody is stopped) | In-flight states `BOOST` and `BURNOUT` are silent on the buzzer. |
| Log conversion started | "Mii Channel" melody, loops until conversion finishes | The post-flight log conversion has the flight recorder (and the SD card). Arming is held off for the whole melody — see {ref}`Arming During a Conversion <arming-during-conv>`. |
| Log conversion complete | Melody stops (or reverts to "Astronomia" if the rocket is still `LANDED`) | Recorder free again; the state machine will arm as soon as the other conditions are met. Raised whether the conversion succeeded or failed. |
| Error | Three 4000 Hz beeps of 100 ms, separated by 100 ms gaps | Unrecoverable error. Service required. |
| Powerfail | *(not handled)* | The buzzer backend does not implement `on_powerfail`. |

:::{note}
The `LANDED` and conversion melodies are the only patterns that run
asynchronously. The `LANDED` beacon is stopped automatically on the
next state transition. The conversion melody outlives state changes
somce it tracks a background job, not a state, so tones raised
while it plays (calibration, state changes) briefly pause it and it
resumes afterwards. Both share one playback context, because the board
has exactly one buzzer.
:::

### Quick Reference

| State | LED | Buzzer |
| --- | --- | --- |
| `IDLE` | Short blink (50 / 450 ms) | 500 Hz · 50 ms |
| `ARMED` | Even blink (200 / 200 ms) | 2000 Hz · 200 ms |
| `BOOST` | Off | Silent |
| `BURNOUT` | Off | Silent |
| `APOGEE` | Off | 3000 Hz · 300 ms |
| `MAIN` | Off | 2500 Hz · 300 ms |
| `REDUNDANT` | Off | 2 × 2500 Hz · 150 ms beeps |
| `LANDED` | Long blink (400 / 100 ms) | "Astronomia" melody (looping) |
| `ERROR` | Solid ON | 3 × 4000 Hz · 100 ms beeps |

Not a state, but audible for as long as it lasts:

| Background job | LED | Buzzer |
| --- | --- | --- |
| Post-flight log conversion | *(unchanged)* | "Mii Channel" melody (looping) |

## Threading and Queueing

The notification dispatcher ({c:func}`notify_state_change`,
{c:func}`notify_error`, ...) runs synchronously in the caller's
thread — it fans out to each backend inline. Individual backends may
choose to offload their work to avoid blocking flight-critical
threads.

**Buzzer backend** runs on a dedicated worker thread with a bounded
FIFO event queue:

- Calls from the state-machine task return immediately (they only
  enqueue an event), so blocking tone sequences (up to ~600 ms for
  the error pattern) never stall the 10 Hz state machine.
- Events are played in FIFO order. Important sequencing — notably
  stopping the `LANDED` melody before a new tone — is preserved
  because the worker thread runs `pwm_melody_stop` and the
  subsequent tone in one dequeued step.
- When the queue is full, new events are dropped with a
  `LOG_WRN`. This gives natural back-pressure: the worker drains
  at the speed of its tone sequences, and bursty producers cannot
  unboundedly queue up noise.

Tunables (under `AURORA_NOTIFY_BUZZER`):

| Kconfig | Default | Purpose |
| --- | --- | --- |
| `AURORA_NOTIFY_BUZZER_QUEUE_SIZE` | 16 | Maximum queued events before overflow drops. |
| `AURORA_NOTIFY_BUZZER_STACK_SIZE` | 1024 | Worker thread stack size (bytes). |
| `AURORA_NOTIFY_BUZZER_THREAD_PRIORITY` | 10 | Worker thread priority. Keep numerically above flight threads (priority 5) so notifications never preempt them. |

**LED backend** does not need a dedicated thread: blinking is
delegated to Zephyr's `pwm-leds` driver (software timer), and the
remaining inline sleeps are short (≤ 500 ms at boot, 50 ms on
calibration) and occur outside the flight hot path.

## API Reference

```{doxygengroup} lib_notify
   :content-only:
```
