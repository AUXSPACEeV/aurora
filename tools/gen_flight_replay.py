#!/usr/bin/env python3
# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0
#
# Convert a recorded aurora flight CSV (as produced by the data logger's
# CSV converter) into a generated C source file with embedded sample
# arrays. Consumed by the replay-mode fake_sensors backend so simulated
# autotests can be driven by real flight data instead of a synthetic
# profile.
#
# Output schema (see fake_sensors_replay.c):
#
#   struct replay_imu_sample  { uint64_t t_ns; float x, y, z; };
#   struct replay_baro_sample { uint64_t t_ns; float pres_kpa, temp_c; };
#
#   const struct replay_imu_sample  replay_accel[REPLAY_ACCEL_LEN];
#   const struct replay_imu_sample  replay_gyro[REPLAY_GYRO_LEN];
#   const struct replay_baro_sample replay_baro[REPLAY_BARO_LEN];

import argparse
import csv
import re
import sys
from pathlib import Path

# Window padding around the [BOOST, LANDED] interval, in nanoseconds.
TRIM_PAD_NS = 4 * 1_000_000_000


def parse_csv(path):
    accel, gyro, baro = [], [], []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                ts = int(row["timestamp_ns"])
            except (KeyError, ValueError):
                continue
            ax = row.get("accel_x", "")
            if ax != "":
                accel.append((ts, float(ax),
                              float(row["accel_y"]),
                              float(row["accel_z"])))
                gx = row.get("gyro_x", "")
                if gx != "":
                    gyro.append((ts, float(gx),
                                 float(row["gyro_y"]),
                                 float(row["gyro_z"])))
            pres = row.get("baro_pres", "")
            if pres != "":
                temp = row.get("baro_temp", "") or "0"
                baro.append((ts, float(pres), float(temp)))
    return accel, gyro, baro


def parse_state_audit(path):
    """Return (boost_ns, landed_ns) parsed from a state_audit file.

    Despite the "Time (ms)" header the recorded values are in ns
    (matching CSV timestamp_ns), so we use them directly.
    """
    boost_ns = None
    landed_ns = None
    line_re = re.compile(r"^\s*(\d+)\s+transition\s+\S+\s+(\S+)\s*$")
    with open(path) as f:
        for line in f:
            m = line_re.match(line)
            if not m:
                continue
            t_ns, to_state = int(m.group(1)), m.group(2)
            if to_state == "BOOST" and boost_ns is None:
                boost_ns = t_ns
            elif to_state == "LANDED":
                landed_ns = t_ns
    if boost_ns is None or landed_ns is None:
        raise ValueError(
            f"state audit {path} has no BOOST/LANDED transition "
            f"(boost={boost_ns}, landed={landed_ns})"
        )
    return boost_ns, landed_ns


def trim_window(samples, lo_ns, hi_ns):
    return [s for s in samples if lo_ns <= s[0] <= hi_ns]


def emit_imu_array(name, samples, t0):
    out = [f"const struct replay_imu_sample {name}[] = {{"]
    for t, x, y, z in samples:
        out.append(f"\t{{ {t - t0}ULL, {x:.6f}f, {y:.6f}f, {z:.6f}f }},")
    out.append("};")
    out.append(f"const size_t {name}_len = ARRAY_SIZE({name});")
    return "\n".join(out)


def emit_baro_array(name, samples, t0):
    out = [f"const struct replay_baro_sample {name}[] = {{"]
    for t, p, temp in samples:
        out.append(f"\t{{ {t - t0}ULL, {p:.6f}f, {temp:.6f}f }},")
    out.append("};")
    out.append(f"const size_t {name}_len = ARRAY_SIZE({name});")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="flights.csv from data_logger")
    ap.add_argument("--output", required=True, help="generated .c output path")
    ap.add_argument("--state-audit",
                    help="state_audit file from the same flight; if given, "
                         "samples are trimmed to "
                         "[BOOST - 4 s, LANDED + 4 s]")
    args = ap.parse_args()

    accel, gyro, baro = parse_csv(args.input)
    if not accel or not gyro or not baro:
        print("error: input CSV is missing accel/gyro/baro samples",
              file=sys.stderr)
        return 1

    trim_note = ""
    if args.state_audit:
        boost_ns, landed_ns = parse_state_audit(args.state_audit)
        lo = boost_ns - TRIM_PAD_NS
        hi = landed_ns + TRIM_PAD_NS
        accel = trim_window(accel, lo, hi)
        gyro = trim_window(gyro, lo, hi)
        baro = trim_window(baro, lo, hi)
        if not accel or not gyro or not baro:
            print("error: trim window is empty - check that --state-audit "
                  "timestamps match CSV timestamp_ns", file=sys.stderr)
            return 1
        pad_s = TRIM_PAD_NS // 1_000_000_000
        trim_note = (f" * Trimmed: BOOST-{pad_s}s ({boost_ns} ns) ... "
                     f"LANDED+{pad_s}s ({landed_ns} ns)\n")

    t0 = min(accel[0][0], gyro[0][0], baro[0][0])

    header = (
        "/* AUTO-GENERATED by tools/gen_flight_replay.py.\n"
        " * Do not edit. Re-run the build to regenerate.\n"
        f" * Source: {Path(args.input).name}\n"
        f" * Samples: accel={len(accel)} gyro={len(gyro)} baro={len(baro)}\n"
        f" * Span:    {(max(accel[-1][0], gyro[-1][0], baro[-1][0]) - t0)/1e9:.3f} s\n"
        f"{trim_note}"
        " */\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <zephyr/sys/util.h>\n"
        '#include "fake_sensors_replay.h"\n'
    )

    body = "\n\n".join([
        emit_imu_array("replay_accel", accel, t0),
        emit_imu_array("replay_gyro", gyro, t0),
        emit_baro_array("replay_baro", baro, t0),
    ])

    Path(args.output).write_text(header + "\n" + body + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
