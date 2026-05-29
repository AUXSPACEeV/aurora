# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Simple HC-12 ground station for telemetry tests.

Runs on a Raspberry Pi Pico flashed with MicroPython, wired to an HC-12
radio module on UART1 (TX=GP4, RX=GP5). The HC-12 receives frames sent
over the air by the AURORA flight computer; this script parses them and
prints a human-readable line per frame to the REPL.

Wire format (must match the Zephyr sender in
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
import time
import struct

# Match the Zephyr-side HC-12 UART baud!
BAUD = 9600

uart = UART(1, baudrate=BAUD, tx=Pin(4), rx=Pin(5),
            rxbuf=2048, timeout=0)

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

SM_STATES = ("IDLE", "ARMED", "BOOST", "BURNOUT",
             "APOGEE", "MAIN", "REDUNDANT", "LANDED", "ERROR")

SM_UPDATE_FMT = "<IBBh7d"
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
    tag = "OK " if crc_ok else "BAD"
    if ftype == HC12_TYPE_SM_UPDATE and plen == SM_UPDATE_LEN and crc_ok:
        (ts, state, armed, _resv,
         altitude, accel, accel_vert, velocity,
         yaw, pitch, roll) = struct.unpack_from(SM_UPDATE_FMT, mv, 0)
        name = SM_STATES[state] if state < len(SM_STATES) else "?%d" % state
        out_lines.append(
            "[OK ] SM t=%d ms  state=%-9s armed=%d  alt=%+.2f  "
            "a=%+.2f  av=%+.2f  v=%+.2f  ypr=%+.2f/%+.2f/%+.2f"
            % (ts, name, armed, altitude, accel, accel_vert, velocity,
               yaw, pitch, roll))
    else:
        out_lines.append("[%s] type=0x%02x len=%d" % (tag, ftype, plen))


def parse():
    """Drain as many complete frames as possible from buf[read_ix:buf_len]."""
    global read_ix
    end = buf_len
    i = read_ix
    while True:
        # Find magic A5 5A starting at i. Inlined for speed: scan for A5,
        # check the following byte.
        while i + 1 < end:
            if buf[i] == MAGIC0 and buf[i + 1] == MAGIC1:
                break
            i += 1
        else:
            # ran out without finding a magic pair; keep last byte
            # in case it's a stray A5 awaiting its 5A.
            if i < end and buf[i] == MAGIC0:
                read_ix = i
            else:
                read_ix = end
            return

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
            # memoryview into the payload region; struct.unpack_from is fine.
            queue_frame(ftype, memoryview(buf)[i + 4:i + 4 + plen],
                        plen, True)
            i += frame_len
        else:
            # CRC bad: report and skip just the magic, in case the real
            # frame starts one byte later.
            queue_frame(ftype, None, plen, False)
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


print("rec_zephyr fast: baud=%d, frame=%d" % (BAUD, 4 + SM_UPDATE_LEN + 2))

while True:
    n = uart.any()
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
