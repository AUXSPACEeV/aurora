#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Extract a file from a running native_sim Zephyr instance.

The Zephyr FUSE bridge (CONFIG_FUSE_FS_ACCESS) opens files write-only, so
`cp` from the FUSE mount fails with EIO. Workaround: dump the file over the
shell console via `fs read <path>` (hex dump format) and reconstruct it on
the host.

Two modes:
  parse      Parse a captured console log and write the bytes of one
             `fs read` invocation to disk.
  pexpect    Spawn a native_sim binary, drive the shell, and fetch a file
             in one step. Requires the `pexpect` package.

Hex dump format emitted by subsys/fs/shell.c cmd_read:
  File size: <n>
  <8 hex digits offset>  <up to 16 "NN " bytes>   <ascii gutter>
  ...

Example use: (after building with `west build -p -b native_sim sensor_board`)
python3 ./tools/sim_fetch.py pexpect \
    build/zephyr/zephyr.exe \
    --await-ready "Attitude calibrated" \
    --pre-command "sim launch" \
    --wait-for "LANDED" \
    --wait-timeout 600 \
    --fetch /RAM:/data/flight_0.influx flight_logs/sim_fetch/flights.influx \
    --fetch /RAM:/state/audit.0 flight_logs/sim_fetch/state_audit \
    -v
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HAS_PEXPECT: bool
try:
    import pexpect
    HAS_PEXPECT = True
except ImportError:
    HAS_PEXPECT = False

HEADER_RE = re.compile(r"^File size:\s*(\d+)\s*$")
LINE_RE = re.compile(
    r"^(?P<offset>[0-9A-Fa-f]{8})\s{2}"
    r"(?P<hex>(?:[0-9A-Fa-f]{2} ){1,16})"
)


def parse_dump(text: str, expected_size: int | None = None) -> bytes:
    """
    Pick the last complete `fs read` hex dump in *text* and return its bytes.

    A dump starts with a "File size: N" line and continues while subsequent
    lines match the hex-dump grammar with a monotonically increasing offset
    that matches the accumulated byte count.
    """
    lines = text.splitlines()
    best: bytes | None = None
    best_size: int | None = None

    i = 0
    while i < len(lines):
        m = HEADER_RE.match(lines[i].strip())
        if not m:
            i += 1
            continue

        size = int(m.group(1))
        i += 1
        buf = bytearray()
        while i < len(lines):
            lm = LINE_RE.match(lines[i])
            if not lm:
                # tolerate blank lines inside the dump
                if lines[i].strip() == "":
                    i += 1
                    continue
                break
            offset = int(lm.group("offset"), 16)
            if offset != len(buf):
                # discontinuity; abandon this dump
                break
            hex_tokens = lm.group("hex").split()
            buf.extend(int(b, 16) for b in hex_tokens)
            i += 1

        if len(buf) == size:
            best = bytes(buf)
            best_size = size
        # else: partial / malformed, keep scanning for another header

    if best is None:
        raise ValueError("no complete `fs read` dump found in input")

    if expected_size is not None and best_size != expected_size:
        raise ValueError(
            f"expected size {expected_size}, dump reports {best_size}"
        )

    return best


def cmd_parse(args: argparse.Namespace) -> int:
    text = Path(args.input).read_text() if args.input != "-" else sys.stdin.read()
    data = parse_dump(text, expected_size=args.expect_size)
    Path(args.output).write_bytes(data)
    print(f"wrote {len(data)} bytes -> {args.output}", file=sys.stderr)
    return 0


def cmd_pexpect(args: argparse.Namespace) -> int:
    if not HAS_PEXPECT:
        print("pexpect mode requires: pip install pexpect", file=sys.stderr)
        return 2

    if not args.fetch:
        print("pexpect mode needs at least one --fetch REMOTE LOCAL", file=sys.stderr)
        return 2

    prompt = args.prompt
    child = pexpect.spawn(args.sim_binary, encoding="utf-8", timeout=args.timeout)
    if args.verbose:
        child.logfile_read = sys.stderr

    child.expect_exact(prompt)

    if args.await_ready:
        # Async boot output (e.g. calibration) may not have arrived by the time
        # the prompt does. Block until the caller-supplied marker shows up
        # before sending pre-commands that depend on it.
        child.expect(args.await_ready, timeout=args.await_timeout)
        # Re-sync on the prompt so the next sendline lands cleanly.
        child.sendline("")
        child.expect_exact(prompt)

    for pre in args.pre_command:
        child.sendline(pre)
        if args.wait_for:
            # The command might not return to the prompt on its own (e.g. a
            # simulator that keeps streaming output). Wait on a user-supplied
            # marker instead, with its own timeout.
            child.expect(args.wait_for, timeout=args.wait_timeout)
        else:
            child.expect_exact(prompt)

    # Drain anything still buffered (e.g. trailing audit lines after the
    # marker) so the fetch transcript is clean. Re-sync on the prompt.
    child.sendline("")
    child.expect_exact(prompt)

    results: list[tuple[str, int]] = []
    for remote, local in args.fetch:
        child.sendline(f"fs read {remote}")
        child.expect_exact(prompt, timeout=args.fetch_timeout)
        transcript = child.before or ""
        try:
            data = parse_dump(transcript)
        except ValueError as exc:
            print(f"fetch {remote}: {exc}", file=sys.stderr)
            child.sendline("kernel reboot cold")
            child.close(force=True)
            return 1
        Path(local).write_bytes(data)
        results.append((local, len(data)))
        print(f"wrote {len(data)} bytes -> {local}", file=sys.stderr)

    child.sendline("kernel reboot cold")
    child.close(force=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    sub = ap.add_subparsers(dest="mode", required=True)

    p_parse = sub.add_parser("parse", help="parse captured console output")
    p_parse.add_argument("-i", "--input", default="-",
                         help="path to captured console log ('-' for stdin)")
    p_parse.add_argument("-o", "--output", required=True,
                         help="destination file on host")
    p_parse.add_argument("--expect-size", type=int, default=None,
                         help="fail unless dump reports this file size")
    p_parse.set_defaults(func=cmd_parse)

    p_px = sub.add_parser("pexpect", help="spawn a sim and fetch files")
    p_px.add_argument("sim_binary", help="path to zephyr.exe native_sim binary")
    p_px.add_argument("--fetch", action="append", nargs=2, default=[],
                      metavar=("REMOTE", "LOCAL"),
                      help="fetch REMOTE (Zephyr path) to LOCAL (host path); "
                           "repeatable")
    p_px.add_argument("--prompt", default="uart:~$ ")
    p_px.add_argument("--timeout", type=int, default=30,
                      help="default pexpect timeout (boot + fs read)")
    p_px.add_argument("--await-ready", default=None, metavar="PATTERN",
                      help="regex to wait for after boot before sending "
                           "--pre-command (e.g. 'Attitude calibrated')")
    p_px.add_argument("--await-timeout", type=int, default=60,
                      help="timeout for --await-ready (seconds)")
    p_px.add_argument("--pre-command", action="append", default=[],
                      metavar="CMD",
                      help="shell command to send before fetching; repeatable")
    p_px.add_argument("--wait-for", default=None, metavar="PATTERN",
                      help="regex to wait for after each --pre-command "
                           "instead of the shell prompt (e.g. 'LANDED')")
    p_px.add_argument("--wait-timeout", type=int, default=300,
                      help="timeout for --wait-for (seconds)")
    p_px.add_argument("--fetch-timeout", type=int, default=120,
                      help="timeout for the `fs read` dump itself (seconds)")
    p_px.add_argument("-v", "--verbose", action="store_true")
    p_px.set_defaults(func=cmd_pexpect)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
