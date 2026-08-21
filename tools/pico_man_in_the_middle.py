# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Ground station.
Runs on a Raspberry Pi Pico flashed with MicroPython, wired to micrometer@2
on UART0 (RX=GP1), HC-12 is on the same UART-channel (UART0, TX=GP0).
GPS-Module (U-Blox Neo-6M) is on UART1 (TX=GP4, RX=GP5).

This script does:
    - receive messages from micrometer on UART0
    - AT-Command? -> Send on UART0 to HC-12
    - telemetry? -> append GPS data, recalc CRC, send to HC-12 on UART0

telemetry format (must match the Zephyr sender in
aurora/lib/telemetry/hc12/hc12.c):

    [A5 5A] [type] [payload_len] [payload...] [crc16_le]

    - magic bytes A5 5A let us re-synchronise after dropped bytes
    - the CRC-16/CCITT (reflected, init 0xFFFF) covers type + len +
      payload, so a corrupted frame is detected and discarded instead of
      being shown as garbage

Design notes for readers new to embedded I/O:

    - The UART driver is given a 2 KB hardware-backed buffer (rxbuf=2048)
      so that incoming bytes keep flowing even while Python is busy
      parsing or printing. Without this, the small on-chip FIFO (32 B)
      overruns during slow operations and bytes are silently lost.
    - Parsing reuses one preallocated bytearray; no per-frame allocation
      means no garbage-collector pauses in the hot path.
    - The CRC is computed via a 256-entry lookup table, which is roughly
      8x faster than the bit-by-bit reference implementation.
    - print() to the REPL is slow and blocking, so output is queued and
      only flushed while the UART is idle. This keeps the parse loop
      tight enough to drain frames in real time.

Bringup checklist:

    1. Flash MicroPython onto the Pico, copy this file as main.py.
    2. Confirm the HC-12 baud matches BAUD below (factory default 9600).
    3. Open the REPL; one line should print per received telemetry frame.
"""

from machine import UART, Pin
import utime, time
import struct

# Match the Zephyr-side HC-12 UART baud!
BAUD = 115200

uart = UART(0, baudrate=BAUD, rxbuf=2048, timeout=0)
uart_gnss = UART(1, baudrate=9600, tx=Pin(4), rx=Pin(5))

# ---- HC-12 wire frame ----
# buf[0..1]  magic       0xA5, 0x5A
# buf[2]     type
# buf[3]     payload_len
# buf[4..]   payload      (payload_len bytes)
# buf[..]    CRC-16/CCITT (init 0xFFFF) over buf[2 .. 4+payload_len-1]
# Reflected variant (poly 0x8408) - matches Zephyr's crc16_ccitt().

MAGIC0 = 0xA5
MAGIC1 = 0x5A
HC12_TYPE_SM_UPDATE = 0x01

# State-name tables keyed by the sm_type byte from the protocol.
# Must match enum sm_type in aurora/include/aurora/lib/state/state.h and the
# implementation-specific sm_state enum it identifies. Append-only. NEVER
# renumber an existing entry, the receiver may be running with a
# firmware that still emits the old ID.
SM_TYPE_SIMPLE = 0
# SM_TYPE_TWO_STAGE = 1
SM_STATE_TABLES = {
    SM_TYPE_SIMPLE: ("IDLE", "ARMED", "BOOST", "BURNOUT",
                     "APOGEE", "MAIN", "REDUNDANT", "LANDED", "ERROR"),
}
SM_TYPE_NAMES = {SM_TYPE_SIMPLE: "simple"}

# Wire format: uint32 ts, uint8 state, uint8 armed, uint8 sm_type,
# uint8 reserved, 7x double. Same 64-byte total as the previous
# (state, armed, int16 reserved, 7d) layout. Note: the two trailing
# scalar bytes were repurposed.
SM_UPDATE_FMT = "<IBBBB7d"
SM_UPDATE_LEN = struct.calcsize(SM_UPDATE_FMT)  # 64

# Precomputed CRC-16/CCITT (reflected, poly 0x8408) table.
def _build_crc_table():
    tbl = []
    for b in range(256):
        c = b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
        tbl.append(c)
    return tbl
_CRC_TABLE = _build_crc_table()


def crc16_ccitt(data, start, end, seed=0xFFFF):
    crc = seed
    t = _CRC_TABLE
    for i in range(start, end):
        crc = (crc >> 8) ^ t[(crc ^ data[i]) & 0xFF]
    return crc


# Ring/scratch buffer. We append into `buf` and walk a read cursor.
# Once the cursor passes a threshold we compact in-place.
BUF_CAP = 4096
buf = bytearray(BUF_CAP)
buf_len = 0   # bytes currently held
read_ix = 0   # next byte to parse

# Pending output (we batch prints to avoid blocking the parse loop).
out_lines = []

def queue_frame(ftype, mv, plen, crc_ok):
    global latitude, longitude, satellites, GPStime

    tag = "OK " if crc_ok else "BAD"

    if ftype == HC12_TYPE_SM_UPDATE and plen == SM_UPDATE_LEN and crc_ok:
        # original telemetry data
        (ts, state, armed, sm_type, _resv,
         altitude, accel, accel_vert, velocity,
         yaw, pitch, roll) = struct.unpack_from(SM_UPDATE_FMT, mv, 0)

        states = SM_STATE_TABLES.get(sm_type)
        name = (states[state] if states and state < len(states)
                else "?%d" % state)
        tname = SM_TYPE_NAMES.get(sm_type, "?%d" % sm_type)

        # GPS data
        lat_f = float(latitude) if latitude else 0.0
        lon_f = float(longitude) if longitude else 0.0
        sats_i = int(satellites) if satellites else 0
        h, m, s = 0, 0, 0

        if GPStime:
            try:
                h, m, s = [int(x) for x in GPStime.split(':')]
            except ValueError:
                pass

        # console
        if latitude and longitude:
            gps_info = f" | GPS: Lat={latitude} Lon={longitude} Sat={satellites}, {h}:{m}:{s}"
        else:
            gps_info = " | GPS: No Fix"

        out_lines.append(
            "[OK ] SM[%s] t=%d ms  state=%-9s armed=%d  alt=%+.2f  "
            "a=%+.2f  av=%+.2f  v=%+.2f  ypr=%+.2f/%+.2f/%+.2f%s"
            % (tname, ts, name, armed, altitude, accel, accel_vert,
               velocity, yaw, pitch, roll, gps_info)
        )

        # append GPS to telemetry data
        # Format "<ffBBBB" = little-endian, 2x 32-bit float, 4x unsigned 8-bit char
        gps_bytes = struct.pack("<ffBBBB", lat_f, lon_f, sats_i, h, m, s)

        new_payload = bytearray(mv) + gps_bytes
        new_len = len(new_payload) # Alte Länge + 12

        header = bytearray([MAGIC0, MAGIC1, ftype, new_len])

        crc_data = bytearray([ftype, new_len]) + new_payload
        new_crc = crc16_ccitt(crc_data, 0, len(crc_data))

        frame_to_send = header + new_payload + struct.pack("<H", new_crc)

        uart.write(frame_to_send)

    else:
        out_lines.append("[%s] type=0x%02x len=%d" % (tag, ftype, plen))


def parse():
    """Drain complete frames or AT commands from buf[read_ix:buf_len]."""
    global read_ix
    end = buf_len
    i = read_ix
    while True:
        is_at = False

        while i + 1 < end:
            if buf[i] == MAGIC0 and buf[i + 1] == MAGIC1:
                break
            if buf[i] == ord('A') and buf[i + 1] == ord('T'):
                is_at = True
                break
            i += 1
        else:
            if i < end and (buf[i] == MAGIC0 or buf[i] == ord('A')):
                read_ix = i
            else:
                read_ix = end
            return

        if is_at:
            newline_idx = -1
            for j in range(i + 2, end):
                if buf[j] in (ord('\n'), ord('\r')):
                    if buf[j] == ord('\r') and j + 1 < end and buf[j + 1] == ord('\n'):
                        newline_idx = j + 1
                    else:
                        newline_idx = j
                    break

            if newline_idx != -1:
                # send AT cmd
                at_cmd = memoryview(buf)[i:newline_idx + 1]
                uart.write(at_cmd)

                i = newline_idx + 1
                continue
            else:
                read_ix = i
                return

        # magic byte parsing
        if end - i < 4:
            read_ix = i
            return

        plen = buf[i + 3]
        frame_len = 4 + plen + 2
        if end - i < frame_len:
            read_ix = i
            return

        ftype = buf[i + 2]
        crc_rx = buf[i + 4 + plen] | (buf[i + 4 + plen + 1] << 8)
        crc_calc = crc16_ccitt(buf, i + 2, i + 4 + plen)

        if crc_rx == crc_calc:
            queue_frame(ftype, memoryview(buf)[i + 4:i + 4 + plen], plen, True)
            i += frame_len
        else:
            i += 2

def compact():
    """Slide unread bytes to the start of the buffer."""
    global buf_len, read_ix
    if read_ix == 0:
        return
    remaining = buf_len - read_ix
    if remaining:
        buf[0:remaining] = buf[read_ix:buf_len]
    buf_len = remaining
    read_ix = 0





gps_line_buffer = ""
FIX_STATUS = False
latitude = ""
longitude = ""
satellites = ""
GPStime = ""

def getGPS(gpsModule):
    global gps_line_buffer, FIX_STATUS, latitude, longitude, satellites, GPStime

    while gpsModule.any():
        raw_bytes = gpsModule.read(gpsModule.any())
        if not raw_bytes:
            break

        try:
            gps_line_buffer += raw_bytes.decode('ascii')
        except UnicodeError:
            gps_line_buffer = ""
            continue

        while '\n' in gps_line_buffer:
            line, gps_line_buffer = gps_line_buffer.split('\n', 1)
            line = line.strip()

            if not line:
                continue

            parts = line.split(',')

            if parts[0] == "$GPGGA" and len(parts) >= 15:
                if parts[2] != '' and parts[4] != '':
                    try:
                        lat_raw = convertToDegree(parts[2])
                        latitude = '-' + lat_raw if parts[3] == 'S' else lat_raw

                        lon_raw = convertToDegree(parts[4])
                        longitude = '-' + lon_raw if parts[5] == 'W' else lon_raw

                        satellites = parts[7]
                        GPStime = parts[1][0:2] + ":" + parts[1][2:4] + ":" + parts[1][4:6]
                        FIX_STATUS = True
                    except Exception as e:
                        pass

def convertToDegree(RawDegrees):

    RawAsFloat = float(RawDegrees)
    firstdigits = int(RawAsFloat/100)
    nexttwodigits = RawAsFloat - float(firstdigits*100)

    Converted = float(firstdigits + nexttwodigits/60.0)
    Converted = '{0:.6f}'.format(Converted)
    return str(Converted)



print("rec_zephyr fast: baud=%d, frame=%d" % (BAUD, 4 + SM_UPDATE_LEN + 2))

while True:
    n = uart.any()
    getGPS(uart_gnss)

    if FIX_STATUS:
        print("\n--- GPS DATA ---")
        print("Latitude:  " + latitude)
        print("Longitude: " + longitude)
        print("Satellites:" + satellites)
        print("Time:      " + GPStime)
        print("----------------")
        FIX_STATUS = False
    if n:
        # Cap read to free space; compact if needed.
        free = BUF_CAP - buf_len
        if n > free:
            compact()
            free = BUF_CAP - buf_len
            if n > free:
                n = free
        if n:
            mv = memoryview(buf)[buf_len:buf_len + n]
            got = uart.readinto(mv, n)
            if got:
                buf_len += got
        parse()
        if read_ix > BUF_CAP // 2:
            compact()
    else:
        # Flush any queued output while idle. Printing here, not in the
        # hot path, is the whole point.
        if out_lines:
            print("\n".join(out_lines))
            out_lines = []
        time.sleep_ms(1)


