# pad_link_central_example.py

Minimal BLE central for the AURORA pad-link.

This script is the smallest thing that talks to a rocket running
`CONFIG_AURORA_PAD_LINK=y`. It scans for the rocket's advertised
service, connects, reads the static characteristics (board name, SM
type, and board capabilities), prints what the board supports, and then
subscribes to / polls only the characteristics that are actually present
according to the capability register.

It is meant as a *reference*, not a finished ground station. Use it to:

- test the rocket-side firmware after enabling the pad link
  (you should see at least the board id, capabilities, and the IDLE state),
- see how to decode the wire layouts documented in
  {doc}`/lib/pad_link` in a real client,
- copy/paste pieces into a fuller UI (a Tk dashboard, a logger,
  the launchrail's own central).

## Wire contract

Mirrors
[aurora/lib/pad_link/pad_link.c](https://github.com/AUXSPACEeV/aurora/tree/main/lib/pad_link/pad_link.c).

```{important}
If the C-side struct or a UUID changes, the constants here must change too.
```

Characteristics:

| Characteristic | Description |
|:---------------|:------------|
| SERVICE_UUID   | primary service to filter on during scan |
| UUID_BOARD     | UTF-8 string, board identifier |
| UUID_SMTYPE    | uint8, identifies which sm_state enum is in use |
| UUID_BOARDCAP  | uint32 LE, capability register (see below) |
| UUID_STATE     | uint8, current SM state (notify-capable) |
| UUID_COMP      | packed struct, computed kinematics (notify-capable) |
| UUID_BARO      | packed struct, baro pressure + temperature (notify-capable) |
| UUID_IMU6      | packed struct, 6-DoF accel + gyro (notify-capable) |
| UUID_INNER_TEMP| packed struct, inner temperature (notify-capable) |
| UUID_RAW       | packed struct, raw IMU+baro — deprecated, not used here |

## Board capabilities

After connecting the script reads `UUID_BOARDCAP` (characteristic `a0`), a
`uint32` little-endian register that describes which sensor characteristics
carry valid data on this specific board:

| Bits   | Field       | Values |
|:-------|:------------|:-------|
| [2:0]  | IMU type    | 0=none, 1=6-DoF, 2=9-DoF |
| [3]    | Accel       | 1 = accelerometer valid (a2) |
| [4]    | Gyro        | 1 = gyrometer valid (a3) |
| [8]    | Baro        | 1 = barometer valid (a1) |
| [9]    | Inner temp  | 1 = inner temperature valid (a7) |
| [10]   | Motor temp  | 1 = motor temperature valid (a8, planned) |
| [11]   | Hull temp   | 1 = hull temperature valid (a9, planned) |
| [16]   | GPS/GNSS    | 1 = GPS valid (a6, planned) |

The script uses this register to decide which optional characteristics to
subscribe to or poll, so it works correctly on both hardware revisions
(µMETER v1 with LPS22HH, µMETER v2 with BMP581 + extra sensors).

## Payload layouts (all little-endian, micro-units ÷ 1 000 000)

```
comp:       struct.unpack("<Iffffff", data[:28])  → uptime_ms, alt_m, vel_m_s, yaw, pitch, roll, az
baro:       struct.unpack("<Iqq",     data[:20])  → uptime_ms, temp_µ°C, press_µPa
imu6:       struct.unpack("<Iqqqqqq", data[:52])  → uptime_ms, ax,ay,az µm/s², gx,gy,gz µrad/s
inner_temp: struct.unpack("<Iq",      data[:12])  → uptime_ms, temp_µ°C
boardcap:   struct.unpack("<I",       data[:4])   → flags
```

## Prerequisites

- Python 3.9+
- `pip install bleak`
- A BLE-capable host (built-in adapter on Linux/macOS/Windows,
  or a plugged-in USB BLE dongle).
- On Linux the user usually needs to be in the `bluetooth` group,
  or the script has to be run with `sudo`.

## Running

```bash
python3 pad_link_central_example.py
```

You should see the board name, a capability summary, and then a stream of
state and telemetry prints. The script exits after 60 seconds; pass
`--duration` to change this.

## Notes for first-time users

- Default ATT MTU (23 B) is enough: `read_gatt_char` and
  notifications transparently do *long reads* for payloads larger
  than the MTU. Negotiating a bigger MTU (`BleakClient(..., mtu=247)`)
  just makes them faster.
- The first read of `UUID_STATE` before the rocket's state
  machine has run even once returns 0 (= IDLE). Values stabilise
  within milliseconds of boot.
- Disconnects are normal and silent. The rocket re-starts
  advertising automatically. Wrap the body in a reconnect loop
  if you want the script to survive them.
