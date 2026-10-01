# Telemetry

The telemetry library is the downlink path for in-flight data. It is a
small dispatcher: every backend that registers a
{c:struct}`telemetry_backend` at link time receives each outgoing
message. Backends own their own framing, transport, worker threads, and
any backend-specific rate limiting.

Two backends ship in-tree, and both are UARTs underneath. Writing
telemetry frames to a UART is the same job whether the far end is an
HC-12 radio bridge or an AUX-Tel relay on the stack connector: pack the
payload, frame it, rate-limit it, and push the bytes from a worker
thread so the caller never blocks. That job is the **uart-link engine**
(`lib/telemetry/uart_link`). What differs per far end - a provisioning
pin, a command set, a modem whose baud rate has to change - stays in the
backend that owns it, which is why the HC-12 backend is that engine plus
a SET pin and an AT helper.

The API stays transport-agnostic: a LoRaWAN, CAN-tunnel or any other
backend can be added without touching the dispatcher or callers, and
without going through the uart-link engine at all.

## Architecture

(telemetry-architecture-diagram)=

```{image} /img/telemetry_architecture.drawio.svg
:alt: telemetry_arch
```

The dispatcher is synchronous and runs in the caller's thread: it walks
the iterable section and invokes each backend's `send_sm_update` hook
inline. Backends that need to do real work (UART writes, radio TX) must
offload to their own worker thread and return immediately.
The dispatcher itself never blocks.

## Backends

- **Plain UART link** (`CONFIG_AURORA_TELEMETRY_UART_LINK`): the
  uart-link engine with nothing bolted on, bound to an
  `auxspaceev,telemetry-uart-link` node. For a peer that already speaks
  the wire format and needs no provisioning - an
  {doc}`AUX-Tel relay </applications/abby>` on the stack data
  connector, or a laptop on a USB-serial adapter.
- **HC-12** (`CONFIG_AURORA_TELEMETRY_HC12`): transparent UART
  ↔ 433 MHz RF bridge, bound to an `auxspaceev,hc12` node. The same
  engine plus a SET pin and an AT helper, so the module can be
  provisioned at runtime as well as on the bench (channel, air baud, TX
  power). See [Provisioning].

Several backends can run at once: a board may push the same updates to a
local HC-12 and to an AUX-Tel relay above it, each on its own UART with
its own rate limit.

### Adding a new backend

A backend is two things: a vtable filled with the operations it
supports, and a single `TELEMETRY_BACKEND_DEFINE` invocation to
register it at link time.

```c
#include <aurora/lib/telemetry.h>

static int my_init(void) { /* ... */ return 0; }

static int my_send_sm_update(enum sm_state state, enum sm_type type,
                             const struct sm_inputs *inputs)
{
    /* Frame and enqueue. Must not block. Return:
     *   0       on accept,
     *   -EAGAIN if throttled,
     *   -ENOMEM if your TX queue is full,
     *   -ENODEV if the transport is not ready.
     */
    return 0;
}

static const struct telemetry_backend_api my_api = {
    .init           = my_init,
    .send_sm_update = my_send_sm_update,
};

TELEMETRY_BACKEND_DEFINE(my_backend, &my_api);
```

Add a `CONFIG_AURORA_TELEMETRY_<BACKEND>` symbol under
`lib/telemetry/Kconfig` and a conditional
`zephyr_library_sources(...)` block in `lib/telemetry/CMakeLists.txt`
to compile it in. No changes to the dispatcher or to callers
(`telemetry_send_sm_update`) are needed.

(telemetry-wire-frame)=
## Wire frame

Every state-machine update becomes a small self-describing packet, so a
receiver can resynchronise after corruption. All multi-byte fields are
little-endian.

The format is defined once, in `include/aurora/lib/telemetry/wire.h`,
and both the senders here and the
{doc}`AUX-Tel relay </applications/abby>` include it. Senders and
receivers are separate firmwares, and a framing mismatch between them
is only visible as silent packet loss - so there is deliberately one
definition rather than a copy per side.

The header carries the framing helpers along with the layout, because
every side needs the same three operations:

| Helper | Used by |
|---|---|
| `telemetry_wire_finalise()` | Senders, to build a frame |
| `telemetry_wire_crc()` | Byte-stream receivers, which have already reassembled a frame and only need its CRC |
| `telemetry_wire_validate()` | Packet receivers, where a whole frame arrives at once |

The split between the last two is about the transport. A UART receiver
reassembles frames from a byte stream and knows the length as it goes, so
it needs the CRC alone. A packet receiver - a LoRa packet, a datagram -
is handed a buffer and a length by something else, and has to establish
that the buffer *is* a frame before trusting anything in it. In
particular, a corrupt `len` byte would otherwise send the CRC helper
reading past what was actually received, so `telemetry_wire_validate()`
cross-checks `len` against the received byte count before computing
anything.

```{eval-rst}
.. list-table::
   :header-rows: 1
   :widths: 15 10 75

   * - Offset
     - Size
     - Field
   * - 0
     - 1
     - magic0 = ``0xA5``
   * - 1
     - 1
     - magic1 = ``0x5A``
   * - 2
     - 1
     - type (see below)
   * - 3
     - 1
     - len (payload bytes that follow, excluding CRC)
   * - 4
     - len
     - payload
   * - 4 + len
     - 2
     - CRC-16/CCITT (init ``0xFFFF``) over bytes ``[2 .. 4+len-1]``
```

Packet types:

```{eval-rst}
.. list-table::
   :header-rows: 1
   :widths: 15 20 65

   * - Value
     - Name
     - Payload
   * - ``0x01``
     - ``SM_UPDATE``
     - State-machine snapshot (see below)
```

`SM_UPDATE` payload (36 bytes):

```{eval-rst}
.. list-table::
   :header-rows: 1
   :widths: 15 15 15 55

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - ``u32``
     - ``timestamp_ms`` (system uptime, low 32 bits)
   * - 4
     - 1
     - ``u8``
     - ``state`` (:c:enum:`sm_state`)
   * - 5
     - 1
     - ``u8``
     - ``armed`` (0/1)
   * - 6
     - 1
     - ``u8``
     - ``sm_type`` (:c:enum:`sm_type` identifies the ``state``
       enum mapping the firmware is using)
   * - 7
     - 1
     - ``u8``
     - reserved (zero)
   * - 8
     - 4
     - ``f32``
     - ``altitude`` (m)
   * - 12
     - 4
     - ``f32``
     - ``acceleration`` (m/s², total)
   * - 16
     - 4
     - ``f32``
     - ``accel_vert`` (m/s², body-vertical)
   * - 20
     - 4
     - ``f32``
     - ``velocity`` (m/s)
   * - 24
     - 12
     - ``f32[3]``
     - ``orientation`` (roll/pitch/yaw, rad)
```

At 10 Hz the link runs at roughly 420 B/s, about 44 % of a 9600-baud
HC-12 air link, leaving headroom for re-tries and other packet types.

## Threading and rate limiting

This is the uart-link engine's behaviour, so it applies to both
backends; each instance has its own queue, worker thread and limit.

Every outgoing frame goes to a dedicated worker thread via a bounded
FIFO message queue. The producer
({c:func}`telemetry_send_sm_update`, called from the state-machine task)
never blocks:

- Frames are framed and CRC'd inline on the caller's stack, then posted
  with `K_NO_WAIT`. A full queue returns `-ENOMEM` and drops the
  frame rather than stalling the SM thread.
- The worker drains the queue and writes bytes with `uart_poll_out`.
  Keeping it on a low-priority thread (default priority 10) ensures
  telemetry can never preempt flight-critical threads (sensors and the
  state machine run at priority 5–6).
- Optional rate limiting drops frames before they touch the queue or
  the UART. Useful when the SM tick rate is higher than the air link
  can carry comfortably (the default 0 disables it).

Each backend exposes the same four tunables under its own prefix -
`AURORA_TELEMETRY_UART_LINK_*` and `AURORA_TELEMETRY_HC12_*` - so two
instances can be tuned independently:

```{eval-rst}
.. list-table::
   :header-rows: 1
   :widths: 55 15 30

   * - Kconfig
     - Default
     - Purpose
   * - ``..._QUEUE_DEPTH``
     - 8 / 16
     - Maximum queued frames before overflow drops.
   * - ``..._MIN_INTERVAL_MS``
     - 0
     - Minimum spacing between accepted SM updates (ms).
       0 = unlimited.
   * - ``..._STACK_SIZE``
     - 1024
     - Worker thread stack size (bytes).
   * - ``..._THREAD_PRIORITY``
     - 10
     - Worker thread priority. Keep numerically above flight threads
       (priority 5) so telemetry never preempts them.
```

## Device-tree

### Plain UART link

`auxspaceev,telemetry-uart-link` (see
`dts/bindings/telemetry/auxspaceev,telemetry-uart-link.yaml`) carries
nothing but a phandle to the UART, because the far end needs nothing
else. A brain board feeding an AUX-Tel relay through the stack
connector:

```dts
/ {
   telemetry_link: telemetry-uart-link {
      compatible = "auxspaceev,telemetry-uart-link";
      uart = <&aux_data_uart_top>;
      status = "okay";
   };
};

&aux_data_uart_top {
   status = "okay";
};
```

Nothing in this backend reconfigures the line, so the node's
`current-speed` has to match what the far end listens at.

### HC-12

The HC-12 backend is bound to a devicetree node with the compatible
`auxspaceev,hc12` (see `dts/bindings/hc12/auxspaceev,hc12.yaml`).
The node carries a phandle to the host UART and, optionally, the
`SET` line:

Minimal node. Transparent downlink only, no runtime AT support:

```dts
/ {
   hc12: hc12 {
      compatible = "auxspaceev,hc12";
      uart = <&uart1>;
      status = "okay";
   };
};
```

Add `set-gpios` to also enable runtime provisioning through the
shell (see [Provisioning shell]):

```dts
hc12: hc12 {
    compatible = "auxspaceev,hc12";
    uart = <&uart1>;
    set-gpios = <&gpio0 6 GPIO_ACTIVE_LOW>;
    status = "okay";
};
```

Boards that do not mount an HC-12 leave the node disabled (or omit it
entirely); the `AURORA_TELEMETRY_HC12` symbol is then unselectable
(`DT_HAS_AUXSPACEEV_HC12_ENABLED` resolves to `n`) and the backend
is compiled out. The default `sensor_board_v2` DTS ships the node
`status = "disabled"` so each application opts in explicitly via an
overlay.

## Provisioning

The HC-12 stores channel, air baud, TX power and FU mode in its own
non-volatile flash. **Settings persist across power cycles**, so the
firmware never sends AT commands at boot: it only configures `SET`
(when wired) to transparent mode and opens the UART. Provision the
module *once*, when something needs to change.

The HC-12 enters AT-command mode while its `SET` pin is pulled low.
In AT mode the module always speaks 9600 baud regardless of the
transparent-mode air baud. There are two ways to provision it:

1. **On the bench** with a USB-serial adapter: works on any board,
   no firmware support required, no risk of stalling the avionics bus.
2. **From the firmware shell**: works on boards whose HC-12 has its
   `SET` line wired to a GPIO and that build with
   `CONFIG_AURORA_TELEMETRY_HC12_SHELL=y`.

### Bench provisioning

Pull `SET` low, connect a USB-serial adapter at 9600 baud, and send:

```{eval-rst}
.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Command
     - Effect
   * - ``AT``
     - Sanity check; module replies ``OK``.
   * - ``AT+B9600``
     - Set air baud (must match host ``current-speed``).
   * - ``AT+Cnnn``
     - Set channel (``001``-``127``). Match the ground module.
   * - ``AT+P8``
     - Maximum TX power (~20 dBm).
   * - ``AT+FU3``
     - Mode 3 (good range/throughput compromise; default).
   * - ``AT+RX``
     - Read back the current settings.
   * - ``AT+DEFAULT``
     - Restore factory defaults.
```

Release `SET` to exit AT mode. The ground-side HC-12 must use the
same channel, baud, and FU mode.

### Provisioning shell

Enable `CONFIG_AURORA_TELEMETRY_HC12_SHELL=y` (which selects
`CONFIG_AURORA_TELEMETRY_HC12_AT=y`) and the `telemetry hc12`
command tree appears on the Zephyr shell:

```{eval-rst}
.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Command
     - Effect
   * - ``telemetry hc12 info``
     - Issue ``AT+RX`` and print channel/baud/power/mode.
   * - ``telemetry hc12 channel <1..127>``
     - ``AT+C<nnn>``.
   * - ``telemetry hc12 baud <rate>``
     - ``AT+B<rate>`` for ``1200|2400|4800|9600|19200|38400|57600|115200``.
       Warns that ``current-speed`` in DT must be updated and the
       board rebooted to keep the host UART in sync.
   * - ``telemetry hc12 power <1..8>``
     - ``AT+P<n>``; ``8`` is +20 dBm (maximum).
   * - ``telemetry hc12 mode <1..4>``
     - ``AT+FU<n>``.
   * - ``telemetry hc12 reset``
     - ``AT+DEFAULT``.
   * - ``telemetry hc12 at <command>``
     - Raw escape hatch, e.g. ``telemetry hc12 at AT+V``.
```

The shell handler:

- **Refuses every command unless the state machine is in IDLE.**
  Reprovisioning while armed would silence the downlink mid-flight
  and stall the UART worker for ~200 ms; that is never the right
  thing to do in any other state.
- **Holds a mutex on the HC-12 UART** for the duration of each AT
  exchange. The TX worker thread blocks on the same mutex per
  outgoing frame, so transparent-mode bytes can never collide with
  an AT command. Frames generated while AT is in progress queue up
  normally and drain when the lock is released.
- **Restores the host UART baud on every exit path** (including
  errors), so a failed `AT+B` cannot leave the host out of sync
  with the radio.

If the `set-gpios` property is absent from the HC-12 node, every
command returns `-ENOTSUP` and prints a hint pointing at the bench
flow.

## Usage from the application

`main.c` calls {c:func}`telemetry_init` once at boot, then
{c:func}`telemetry_send_sm_update` once per state-machine tick:

```c
#if defined(CONFIG_AURORA_TELEMETRY)
    (void)telemetry_init();
#endif

/* ... inside the state-machine loop, after sm_update(): */
#if defined(CONFIG_AURORA_TELEMETRY)
    struct sm_inputs sm_in;
    sm_get_inputs(&sm_in);
    (void)telemetry_send_sm_update(state, &sm_in);
#endif
```

The return value of {c:func}`telemetry_send_sm_update` is the first
non-zero error any backend returned; remaining backends are still
called. Callers in the hot path typically ignore it.

## API Reference

```{doxygengroup} lib_telemetry
:content-only:
```
