# pad_link_central_example.py

Minimal BLE central for the AURORA pad-link.

This script is the smallest thing that talks to a rocket running
`CONFIG_AURORA_PAD_LINK=y`. It scans for the rocket's advertised
service, connects, reads the two static characteristics (board name
and SM type), and then subscribes to the live ones (SM state and
computed kinematics) so each new value is printed as it arrives.

It is meant as a *reference*, not a finished ground station. Use it to:

- test the rocket-side firmware after enabling the pad link
(you should see at least the board id and the IDLE state),
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
| SERVICE_UUID | primary service to filter on during scan |
| UUID_BOARD   | UTF-8 string, board identifier (e.g. board name) |
| UUID_SMTYPE  | uint8, identifies which sm_state enum is in use |
| UUID_STATE   | uint8, current SM state (notify-capable) |
| UUID_COMP    | packed struct, computed kinematics (notify-capable) |
| UUID_RAW   | packed struct, raw IMU+baro. not used here (optional) |

Computed payload (28 bytes, little-endian):

```c
uint32_t uptime_ms, float altitude_m, float velocity_m_s,
float yaw_deg, float pitch_deg, float roll_deg, float accel_vert_m_s2
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

You should see one connection line and then a stream of "state:" and
"t=..." prints. The script exits after 60 seconds; raise the
`asyncio.sleep` argument or wrap the body in a `while True` for
longer sessions.

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
