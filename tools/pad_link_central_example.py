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

# Capability register (boardcap characteristic, uint32 LE).
# Mirrors include/aurora/lib/pad_link.h. Keep in sync.
#
# Byte 0 — IMU group
CAP_IMU_TYPE_MASK = 0x7
CAP_IMU_TYPE_NONE = 0x0
CAP_IMU_TYPE_6DOF = 0x1
CAP_IMU_TYPE_9DOF = 0x2
CAP_ACCEL         = (1 << 3)
CAP_GYRO          = (1 << 4)
# Byte 1 — Environmental
CAP_BARO          = (1 << 8)
CAP_TEMP_INNER    = (1 << 9)
CAP_TEMP_MOTOR    = (1 << 10)
CAP_TEMP_HULL     = (1 << 11)
# Byte 2 — Positioning
CAP_GPS           = (1 << 16)

SM_STATES = {
    0: ("IDLE", "ARMED", "BOOST", "BURNOUT",
        "APOGEE", "MAIN", "REDUNDANT", "LANDED", "ERROR"),
}

# matches CONFIG_AURORA_PAD_LINK_DEVICE_NAME
DEVICE_NAME = "AURORA-Rocket"


def print_capabilities(cap):
    imu_type = cap & CAP_IMU_TYPE_MASK
    imu_names = {
        CAP_IMU_TYPE_NONE: "none",
        CAP_IMU_TYPE_6DOF: "6-DoF (accel + gyro)",
        CAP_IMU_TYPE_9DOF: "9-DoF (accel + gyro + mag)",
    }
    print("capabilities:")
    print(f"  IMU:        {imu_names.get(imu_type, f'unknown (0x{imu_type:x})')}")
    print(f"  Barometer:  {'present' if cap & CAP_BARO       else 'not present'}")
    print(f"  Inner temp: {'present' if cap & CAP_TEMP_INNER else 'not present'}")
    print(f"  Motor temp: {'present' if cap & CAP_TEMP_MOTOR else 'not present'}")
    print(f"  Hull temp:  {'present' if cap & CAP_TEMP_HULL  else 'not present'}")
    print(f"  GPS/GNSS:   {'present' if cap & CAP_GPS        else 'not present'}")


def decode_baro(data):
    _, temp_us, press_us = struct.unpack("<Iqq", data[:20])
    return (f"baro: temp={temp_us / 1e6:.2f}°C  "
            f"press={press_us / 1e6:.2f} Pa")


def decode_imu6(data):
    _, ax, ay, az, gx, gy, gz = struct.unpack("<Iqqqqqq", data[:52])
    return (f"imu6: a=[x={ax/1e6:+.3f}, y={ay/1e6:+.3f}, z={az/1e6:+.3f}] m/s²  "
            f"g=[x={gx/1e6:+.3f}, y={gy/1e6:+.3f}, z={gz/1e6:+.3f}] rad/s")


def decode_inner_temp(data):
    _, temp_us = struct.unpack("<Iq", data[:12])
    return f"inner_temp: {temp_us / 1e6:.2f}°C"


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


async def run_notify(c, sm_type, cap, duration):
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
    await c.start_notify(UUID_COMP,  on_comp)

    if cap & CAP_BARO:
        await c.start_notify(UUID_BARO,
                             lambda _, d: print(decode_baro(d)))
    if (cap & CAP_IMU_TYPE_MASK) >= CAP_IMU_TYPE_6DOF:
        await c.start_notify(UUID_IMU6,
                             lambda _, d: print(decode_imu6(d)))
    if cap & CAP_TEMP_INNER:
        await c.start_notify(UUID_INNER_TEMP,
                             lambda _, d: print(decode_inner_temp(d)))

    await asyncio.sleep(duration)


async def run_poll(c, sm_type, cap, interval, duration):
    name = SM_STATES.get(sm_type, ())
    deadline = asyncio.get_event_loop().time() + duration
    while asyncio.get_event_loop().time() < deadline:
        state = (await c.read_gatt_char(UUID_STATE))[0]
        comp  = await c.read_gatt_char(UUID_COMP)
        ts, alt, vel, yaw, pitch, roll, az = struct.unpack(
            "<Iffffff", comp[:28])
        s_name = name[state] if state < len(name) else state
        print(f"state={s_name}  t={ts}  alt={alt:+.1f}  v={vel:+.1f}  "
              f"ypr={yaw:+.1f}/{pitch:+.1f}/{roll:+.1f}")

        if cap & CAP_BARO:
            print(decode_baro(await c.read_gatt_char(UUID_BARO)))
        if (cap & CAP_IMU_TYPE_MASK) >= CAP_IMU_TYPE_6DOF:
            print(decode_imu6(await c.read_gatt_char(UUID_IMU6)))
        if cap & CAP_TEMP_INNER:
            print(decode_inner_temp(await c.read_gatt_char(UUID_INNER_TEMP)))

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
        cap     = struct.unpack("<I",
                                await c.read_gatt_char(UUID_BOARDCAP))[0]
        print(f"connected to {board} (sm_type={sm_type})")
        print_capabilities(cap)

        if args.mode == "notify":
            await run_notify(c, sm_type, cap, args.duration)
        else:
            await run_poll(c, sm_type, cap, args.interval, args.duration)

asyncio.run(main())
