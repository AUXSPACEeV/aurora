#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Minimal BLE central for the AURORA pad-link.
"""

import asyncio
import struct
from bleak import BleakScanner, BleakClient

SERVICE_UUID = "e8a59100-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_BOARD   = "e8a59101-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_STATE   = "e8a59102-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_COMP    = "e8a59104-7c0e-4b5b-9a4c-1f1b6f7c4d70"
UUID_SMTYPE  = "e8a59105-7c0e-4b5b-9a4c-1f1b6f7c4d70"

SM_STATES = {
    0: ("IDLE", "ARMED", "BOOST", "BURNOUT",
        "APOGEE", "MAIN", "REDUNDANT", "LANDED", "ERROR"),
}

# matches CONFIG_AURORA_PAD_LINK_DEVICE_NAME
DEVICE_NAME = "AURORA-Rocket"

async def main():
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
        await asyncio.sleep(60)

asyncio.run(main())
