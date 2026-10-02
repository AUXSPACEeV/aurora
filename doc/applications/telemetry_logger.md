# Telemetry Logger

**Source:** [`telemetry_logger`](https://github.com/AUXSPACEeV/aurora/tree/main/telemetry_logger)

`telemetry_logger` is the ground-side recorder that runs on an AUX-Core
(`core65`). It sits in an AUX-Stack next to an AUX-Tel running
{doc}`Abby <abby>` in its **receive** role, reads the telemetry frames Abby
writes to the stack data connector, and writes every valid frame to the SD
card.

It records **unconditionally**: there is no state machine, no arming and no
flight window. Recording starts at boot and runs until power is cut. On
the ground, the part of a flight you care about is exactly the part a gate
would have to guess at. The application reads no sensors and makes no
decisions. {doc}`sensor_board <sensor_board>` does that on the vehicle.

```
 vehicle                                ground
 sensor_board ─UART─▶ abby (relay) ~~LoRa~~▶ abby (receive) ─UART─▶ telemetry_logger ─▶ SD
```

## What ends up on the card

Each recording is a pair of files in `/MMC:/RX` that share an index:

| File | Contents |
|---|---|
| `RX_<n>.BIN` | The frames **verbatim**, back to back: the same byte stream Abby put on the UART. This is the authoritative record, and any tool that parses the {doc}`telemetry wire format </lib/telemetry>` parses this file. |
| `RX_<n>.CSV` | One decoded row per frame, starting with `rx_ms`, the uptime at which the frame arrived. This is only a convenience and can be turned off with `CONFIG_TELEMETRY_LOGGER_CSV=n`. |

CSV columns:
`rx_ms,type,len,tx_ms,state,armed,sm_type,flags,altitude,acceleration,accel_vert,velocity,yaw,pitch,roll`.
SM_UPDATE rows fill everything except `flags`. STATUS rows fill
`tx_ms,state,armed,sm_type,flags`, with `armed` taken from the flag bit. A
frame of an unknown type, or one whose length does not match its type's
layout, gets a row with only `rx_ms,type,len` filled in. It is still in the
`.BIN` in full. A frame is decoded only when its length matches exactly,
because decoding a different wire revision by offset produces numbers that
look plausible but are wrong.

A new index is used each time a recording is opened: at boot, and after
every write error. Old recordings are never overwritten or appended to.

## Behaviour without a card

The recorder owns the mount. If no card is inserted, the card is unformatted
or a write fails, it unmounts the card and retries every
`CONFIG_TELEMETRY_LOGGER_RETRY_MS`. A card inserted after boot is picked up
without a reset. Frames that arrive while nothing is open are counted as
**unrecorded**. They are not queued, because a full queue would end up
discarding the freshest frames.

Files are synced at least every `CONFIG_TELEMETRY_LOGGER_SYNC_INTERVAL_MS`
(1 s by default). FAT commits a file's size only on sync, so this sets how
much data a hard power-off can lose.

## Architecture

| Thread | File | Job |
|---|---|---|
| link (prio 7) | `link.c` | UART interrupt → ring buffer → frame parser (magic + CRC). Payload-agnostic: the same parser as Abby's relay role. |
| recorder (prio 9) | `recorder.c` | Drains a queue of whole frames and writes them to the SD card. Owns the mount, the files and the sync. |

The parser runs at a higher priority than the recorder, so an SD stall never
causes UART overruns. The queue between the two threads holds
`CONFIG_TELEMETRY_LOGGER_QUEUE_DEPTH` frames (16 by default, which covers
about 8 s at the brain board's 2 Hz). Frames that arrive while the queue is
full are counted as **dropped**.

The UART comes from the `uart-bus` of the `auxspace,stack-connector` chosen
node (the top connector by default, see
`boards/core65_esp32s3_procpu.overlay`). The SD card comes from
`auxspace,ffs`. The SD activity LED is the notify library's disk LED on
`auxspace,disk-led`. The notify core and the disk LED build without the
flight state machine; the buzzer and PWM LED backends need it and stay off.

## Building and running

```
west build -b core65/esp32s3/procpu --sysbuild telemetry_logger
west flash
```

For bench work, add `-- -DEXTRA_CONF_FILE=debug.conf` to get per-frame
debug logs and immediate logging.

The shell is on the USB-C console:

- `tlog status` prints the link counters (frames, CRC errors, overruns) and
  the recorder state (open file, bytes, recorded, unrecorded, dropped, write
  errors).
- `tlog test` records a synthetic STATUS frame through the same queue.
  This tests the SD path with nothing attached to the stack connector.
- `fs ls /MMC:/RX` and `fs read` let you look at recordings without
  removing the card.

A heartbeat line with the same counters is logged every
`CONFIG_TELEMETRY_LOGGER_HEARTBEAT_SEC` (30 s).

```{note}
Both ends of the stack UART run at the board default of 115200 baud. If
`tlog status` shows CRC errors climbing while frames are also arriving,
check that the two boards agree on the baud rate before you suspect the
radio.
```
