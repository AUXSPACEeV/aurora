# Abby

**Source:** [`abby`](https://github.com/AUXSPACEeV/aurora/tree/main/abby)

`Abby` is the telemetry bridge that runs on
{doc}`AUX-Tel </boards/auxspace/AUX-Tel/stm32wl/doc/tel65>`. AUX-Tel sits in
the AUX-Stack next to a brain board and owns the long-range link, so `Abby`
is a bridge rather than a flight computer: it moves telemetry frames
between the stack data connector's UART and the LoRa radio.

It runs no state machine, reads no sensors and makes no flight decisions.
{doc}`sensor_board <sensor_board>` does all of that on AUX-Core; `Abby`
only moves its words further.

## The two roles

`CONFIG_ABBY_ROLE` picks the direction. The roles are mirror images, and
two AUX-Tel boards flashed with one each are the two ends of a link:

| Role | Direction | Where it runs |
|---|---|---|
| `ABBY_ROLE_RELAY` (default) | stack UART in → LoRa out | On the vehicle, next to a brain board |
| `ABBY_ROLE_RECEIVE` | LoRa in → stack UART out | On the ground |

In the receive role every packet that validates is written verbatim to the
stack connector's UART, so whatever is attached there - a brain board, or a
USB-serial adapter on a bench - sees exactly the byte stream a directly
wired sender would have produced. That is what makes the roles compose:
a receive-role board is a drop-in stand-in for a cable.

```{important}
Both roles are built from the same radio Kconfig, and that is deliberate.
A LoRa packet only demodulates when both ends agree on frequency,
spreading factor and bandwidth, and a mismatch is **indistinguishable from
no transmitter at all** - no error, no counter, nothing on the air as far
as the receiver can tell. Building both ends from one application keeps
them in step instead of leaving it to two separate sets of constants.

The same trap applies to anything else listening, including Zephyr's
`samples/drivers/lora/receive`, which hardcodes 865.1 MHz and SF10 - 
neither of which matches `Abby`'s defaults. `abby status` prints the radio
parameters so two boards can be compared at a glance.
```

## Why UART and not CAN

AUX-Tel carries an MCP2515 CAN controller on the stack's CAN pair, and CAN
was the original plan for this link. The link is UART instead, for two
reasons: the stack UART reaches the Daphnis-I module directly, whereas the
CAN controller needs
{ref}`two netlist fixes <required-rev-01-fixes>` that rev 01 does not have;
and the telemetry library's
{doc}`uart-link engine </lib/telemetry>` already frames its data for a
transparent UART bridge, so an HC-12 and an AUX-Tel are interchangeable
from the sender's point of view.

The CAN controller is still described in devicetree and left disabled. See
the board doc if you want it.

## Relaying verbatim

`Abby` never looks inside a payload. It validates the frame and passes the
bytes on unchanged, which buys three things:

- A ground station that can already decode an HC-12 downlink decodes this
  one with no changes - the bytes are identical, only the radio differs.
- A new packet type travels end to end without touching this application.
- There is nothing here to keep in sync with the sender beyond the framing
  itself, and that lives in one shared header
  (`include/aurora/lib/telemetry/wire.h`) which both the
  {doc}`HC-12 backend </lib/telemetry>` and this application include. A
  framing mismatch between two separately built firmwares is only visible
  as silent packet loss over RF, so there is deliberately one definition.

## Architecture

Each transport owns a file: `link.c` is the stack connector UART,
`downlink.c` is the radio. Each is a source in one role and a sink in the
other, so the role only decides which way frames flow between them.

### Relay role

Two threads, one slot between them.

```text
  stack connector UART            LoRa
  (LPUART1, 115200 8N1)           (Daphnis-I sub-GHz)
          |                               ^
          v                               |
   [UART ISR] -> ring buffer -> [link thread]        [radio thread]
                                    |                      ^
                                parse frame,                |
                                check CRC  --> newest-frame slot
```

`link.c` drains the UART from its interrupt into a ring buffer and
reassembles frames in a worker thread. `downlink.c` transmits. Between them
sits a **single frame slot, not a queue**: if a frame is already waiting
when a newer one arrives, the older one is dropped. A telemetry downlink
wants the freshest state, and a deeper queue would only let the radio fall
further behind real time. The `superseded` counter records how often that
happens.

### Receive role

One thread, and no buffering at all.

```text
            LoRa                  stack connector UART
  (Daphnis-I sub-GHz)             (LPUART1, 115200 8N1)
          |                               ^
          v                               |
   [radio thread] -- validate frame --> write out
```

A LoRa packet carries its own boundaries, so there is no byte stream to
resynchronise and no partial frame to reassemble: a received packet either
is a frame or is not one. The worker blocks in `lora_recv()`, validates
what arrives with `telemetry_wire_validate()`, and writes it straight out
of the UART - about 6 ms for a 70-byte frame at 115200 baud. Packets
arriving during that write are missed, which costs nothing in practice
because the duty-cycle budget at the far end puts seconds between them.

There is no duty cycle on listening, so none of the budgeting below
applies in this role, and its Kconfig options are hidden.

```{note}
The radio's own CRC has already discarded corrupted packets before
`Abby` sees them. So a climbing `rejected` count is not usually
interference - it is more likely a *different* transmitter on the same
channel, whose packets are intact but are not telemetry frames.
```

### Airtime is the budget

Frame rate is not the constraint - time on air is. A 70-byte SM_UPDATE
frame at SF7/BW125 is roughly 130 ms on air, and a 1 % duty cycle then
allows one frame about every 13 seconds. At SF12 the same frame is over
three seconds, so the same limit allows one frame every five minutes.

`Abby` therefore budgets rather than paces: after each transmission it
computes the next permitted start time from the radio driver's own
`lora_airtime()` and `CONFIG_ABBY_DUTY_CYCLE_PERMILLE`, so the budget
tracks the configured spreading factor, bandwidth and actual frame length
without anyone maintaining a table. The wait happens *before* a frame is
taken out of the slot, so what finally goes out is the freshest frame at
transmit time.

If you need a faster downlink, shrink the frame or lower the spreading
factor. `CONFIG_ABBY_DUTY_CYCLE_ENFORCE=n` exists for bench work and will
transmit as fast as frames arrive, which is almost certainly outside what
your ISM band permits.

```{note}
Check the channel before raising the budget. The Daphnis-I user manual's
channel assignment table lists the EN 300 220 limits per channel, and some
allow more than 1 %. `CONFIG_ABBY_LORA_FREQ_HZ` defaults to 868.1 MHz, a
1 % channel.
```

## Status LEDs

Each LED belongs to one transport, and means the same thing in both roles:
that transport just carried a frame.

| LED | Meaning |
|---|---|
| D3 (`led0`, `LED_1`) | The radio was used - a transmission in the relay role, a received frame in the receive role |
| D2 (`led1`, `LED_2`) | A frame crossed the stack connector |

So in the relay role you expect D2 then D3, and in the receive role D3
then D2. If the first of the pair pulses and the second never follows,
frames are arriving but not getting out - check `abby status`. If neither
pulses, nothing is arriving at all.

## Building and running

The relay role is the default:

```bash
west build -p -b tel65 abby
west flash
```

The receive role is the same application with one option, kept in
`receive.conf` so the radio settings stay shared:

```bash
west build -p -b tel65 abby -- -DEXTRA_CONF_FILE=receive.conf
west flash
```

A factory module needs unlocking before its first flash; see
{doc}`the board doc </boards/auxspace/AUX-Tel/stm32wl/doc/tel65>`.

Console and shell come out on the J5 debug header (USART1), deliberately
separate from the stack UART that carries the frames.

### Proving a board on its own

`abby test [payload_len]` builds a correctly framed synthetic frame and
pushes it into whichever output the role owns, so each half can be proven
before the other end of it exists. In the relay role that is the radio:

```text
uart:~$ abby test
queued a 22 byte test frame for the radio; a duty-cycle wait may delay it
uart:~$ abby status
role              : relay (stack UART -> LoRa)
radio             : 868100000 Hz, SF7, BW125 kHz, CR4/5, private sync
stack link (in):
  frames accepted : 0
  CRC errors      : 0
  ring overruns   : 0 B
lora radio (out):
  frames sent     : 1
  send errors     : 0
  superseded      : 0
  oversize        : 0
  last airtime    : 62 ms
  duty cycle      : enforced (1.0%)
```

A successful `frames sent` confirms radio configuration, the RF front end
and the duty-cycle path without any of the stack wiring. Note that
`frames accepted` stays at zero: `abby test` bypasses the parser by
design, so it proves the output and says nothing about the input.

In the receive role the same command writes to the stack UART instead, so
a USB-serial adapter on the connector's RXD pin shows the frame bytes
without anything being on the air.

```{warning}
Repeating `abby test` in the relay role mostly increments `superseded`,
not `frames sent`. Only the first frame goes out promptly; the rest
replace each other in the single frame slot while the duty-cycle budget
runs down. That is the budget working, not a fault.
```

### Proving the two halves together

Two AUX-Tel boards, one of each role, is a complete self-test - and the
only one that exercises the RF front end in both directions:

```text
board A (relay)                    board B (receive)
uart:~$ abby test                  uart:~$ abby status
                                   role              : receive (LoRa -> stack UART)
                                   lora radio (in):
                                     frames received : 1
                                     rejected        : 0
                                     recv errors     : 0
                                     last RSSI       : -41 dBm
                                     last SNR        : 9 dB
                                   stack link (out):
                                     frames written  : 1
```

If board A reports `frames sent` but board B reports nothing at all, the
radios are not agreeing. Compare the `radio` lines first. If those match,
the next suspect is the RF switch GPIOs in `daphnis1.dtsi` - see the
residual risk noted in
{doc}`the board doc </boards/auxspace/AUX-Tel/stm32wl/doc/tel65>`. A
transmit-only or receive-only failure points there specifically, since
`tx-enable` and `rx-enable` are opposite polarities of the same pin.

### Feeding the relay from a serial adapter

To exercise the relay's input half, feed frames into the stack connector's
UART pins from a USB-serial adapter at 115200 8N1 and watch
`frames accepted` climb. Bytes that are not valid frames are expected to
be ignored: the parser resynchronises on the magic pair and the CRC.

```{note}
A truncated frame can swallow the one after it. The parser is left
mid-payload and consumes a frame's worth of bytes before it can look for a
sync pair again. This is inherent to magic-plus-CRC framing, and harmless
here - the sender writes whole frames under a mutex, and the duty-cycle
budget puts seconds between them.

The receive role has no equivalent weakness, because a LoRa packet is
delivered whole or not at all.
```

## The sending side

In the relay role, the other half of the link is the
{doc}`plain UART link backend </lib/telemetry>`
(`CONFIG_AURORA_TELEMETRY_UART_LINK`) on the brain board. AUX-Core's
`sensor_board` configuration already carries it:

```dts
telemetry_link: telemetry-uart-link {
	compatible = "auxspaceev,telemetry-uart-link";
	uart = <&aux_data_uart_top>;
	status = "okay";
};
```

with `CONFIG_AURORA_TELEMETRY=y` and
`CONFIG_AURORA_TELEMETRY_UART_LINK_MIN_INTERVAL_MS=500`. Swap the phandle
for `aux_data_uart_bottom` if AUX-Tel is stacked below instead.

That 500 ms is worth understanding. The state machine calls
`telemetry_send_sm_update()` on every update - the sensor rate, far more
than a 1 % duty cycle can carry. Sending at 2 Hz costs about 1 % of the
UART and bounds how stale a relayed frame can be to half a second, which
is as fresh as the downlink can make use of. Anything faster is work the
relay throws away, and shows up as a climbing `superseded` count.

```{note}
A consequence worth being clear about: state transitions between
transmit windows are not downlinked. With roughly one frame every 13
seconds and newest-wins at both ends, a short-lived state such as
``BOOST`` can pass unseen. The downlink is for live situational
awareness; the flight recorder on the brain board is the authoritative
record.
```

A receive-role board on the ground is the mirror of this: it writes the
same frames back out of a stack connector, so a ground-side brain board
can consume them with the same parser a wired sender would have fed.
