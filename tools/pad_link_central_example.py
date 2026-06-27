#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Minimal BLE central for the AURORA pad-link.
"""

import argparse
import asyncio
import struct
from bleak import BleakScanner, BleakClient

SERVICE_UUID    = "e8a59100-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_BOARD      = "e8a59101-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_STATE      = "e8a59102-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_RAW        = "e8a59103-7c0e-4b5b-9a4c-1f1b6f7c4d70"  # deprecated
UUID_COMP       = "e8a59104-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_SMTYPE     = "e8a59105-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_BOARDCAP   = "e8a591a0-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_BARO       = "e8a591a1-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_ACCEL      = "e8a591a2-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_GYRO       = "e8a591a3-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_IMU6       = "e8a591a4-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_INNER_TEMP = "e8a591a7-7c0e-4b5b-9a4c-1f1b6f7c4d70"

# Board capability flags (boardcap characteristic, uint32 LE)
BOARDCAP_IMU  = (1 << 0)  # accel (a2), gyro (a3), 6-DoF IMU (a4) available
BOARDCAP_BARO = (1 << 1)  # baro (a1), inner_temp (a7) available

# Decode helpers (all little-endian, values in micro-units → divide by 1e6):
#   baro:       struct.unpack("<Iqq",      data[:20]) → uptime_ms, temp_us, press_us
#   accel:      struct.unpack("<Iqqq",     data[:28]) → uptime_ms, x_us, y_us, z_us
#   gyro:       struct.unpack("<Iqqq",     data[:28]) → uptime_ms, x_us, y_us, z_us
#   imu6:       struct.unpack("<Iqqqqqq",  data[:52]) → uptime_ms, ax,ay,az,gx,gy,gz
#   inner_temp: struct.unpack("<Iq",       data[:12]) → uptime_ms, temp_us
#   boardcap:   struct.unpack("<I",        data[:4])  → flags
#
# Example: temp_c = temp_us / 1_000_000

SM_STATES = {
    0: ("IDLE", "ARMED", "BOOST", "BURNOUT",
        "APOGEE", "MAIN", "REDUNDANT", "LANDED", "ERROR"),
}

# matches CONFIG_AURORA_PAD_LINK_DEVICE_NAME
DEVICE_NAME = "AURORA-Rocket"

def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mode", choices=("notify", "poll"), default="notify",
                   help="notify: subscribe to GATT notifications (peripheral "
                        "pushes on every SM tick). poll: read characteristics "
                        "on a timer (central drives the rate).")
    p.add_argument("--interval", type=float, default=1.0,
                   help="Poll interval in seconds (poll mode only).")
    p.add_argument("--duration", type=float, default=60.0,
                   help="How long to stay connected, in seconds.")
    return p.parse_args()


async def run_notify(c, sm_type, duration):
    def on_state(_, data):
        state = data[0]
        name = SM_STATES.get(sm_type, ())
        print("state:", name[state] if state < len(name) else state)

    def on_comp(_, data):
        ts, alt, vel, yaw, pitch, roll, az = struct.unpack(
            "<Iffffff", data[:28])
        print(f"t={ts}  alt={alt:+.1f}  v={vel:+.1f}  "
              f"ypr={yaw:+.1f}/{pitch:+.1f}/{roll:+.1f}")

    await c.start_notify(UUID_STATE, on_state)
    await c.start_notify(UUID_COMP, on_comp)
    await asyncio.sleep(duration)


async def run_poll(c, sm_type, interval, duration):
    name = SM_STATES.get(sm_type, ())
    deadline = asyncio.get_event_loop().time() + duration
    while asyncio.get_event_loop().time() < deadline:
        state = (await c.read_gatt_char(UUID_STATE))[0]
        comp = await c.read_gatt_char(UUID_COMP)
        ts, alt, vel, yaw, pitch, roll, az = struct.unpack(
            "<Iffffff", comp[:28])
        s_name = name[state] if state < len(name) else state
        print(f"state={s_name}  t={ts}  alt={alt:+.1f}  v={vel:+.1f}  "
              f"ypr={yaw:+.1f}/{pitch:+.1f}/{roll:+.1f}")
        await asyncio.sleep(interval)


async def main():
    args = parse_args()
    # 15 s window: ESP32-S3 advertising is bursty enough that BlueZ's
    # default 5 s sometimes misses everything in a single sweep.
    seen = await BleakScanner.discover(timeout=15.0, return_adv=True)
    # Match by name as well as by service UUID: pad_link puts the
    # 128-bit service UUID in the scan response, which some BlueZ
    # versions don't surface in AdvertisementData.service_uuids.
    dev = next(
        (d for d, ad in seen.values()
         if SERVICE_UUID.lower() in (ad.service_uuids or [])
            or (ad.local_name or d.name or "") == DEVICE_NAME),
        None,
    )
    if dev is None:
        print("No AURORA-Rocket found. Devices seen during scan:")
        if not seen:
            print("  (none)")
        for d, ad in seen.values():
            name = ad.local_name or d.name or "<unnamed>"
            svcs = ", ".join(ad.service_uuids or []) or "-"
            print(f"  {d.address}  rssi={ad.rssi:>4} dBm  "
                  f"name={name!r}  services=[{svcs}]")
        raise SystemExit(
            f"Looking for service {SERVICE_UUID}: not advertised by any "
            "device above. Is the rocket powered, advertising, and in "
            "range? Is CONFIG_AURORA_PAD_LINK=y in the firmware build?")

    async with BleakClient(dev) as c:
        board   = (await c.read_gatt_char(UUID_BOARD)).decode()
        sm_type = (await c.read_gatt_char(UUID_SMTYPE))[0]
        print(f"connected to {board} (sm_type={sm_type})")

        if args.mode == "notify":
            await run_notify(c, sm_type, args.duration)
        else:
            await run_poll(c, sm_type, args.interval, args.duration)

asyncio.run(main())
