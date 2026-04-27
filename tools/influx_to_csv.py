#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Convert InfluxDB line protocol telemetry to CSV.

Each input line has the form:
    telemetry,type=<T> field1=v1,field2=v2 <ts_ns>

The rocket emits one cluster of samples (baro/accel/gyro/sm_*) every flight
loop, with timestamps within the cluster a few hundred microseconds apart and
~10 ms gaps between clusters. This tool groups all measurements whose
timestamps fall within --window-ns of the first sample in a group into a
single CSV row, so one row holds one full snapshot across all sensors.

Columns are named "<type>_<field>" (e.g. accel_x, baro_pres). Missing values
in a group are written as empty cells. The "timestamp_ns" column is the
timestamp of the first sample in the group.
"""

import argparse
import csv
import sys
from pathlib import Path


def parse_line(line):
        """Parse one InfluxDB line.

        Returns (type, {field: float}, ts_ns) or None if the line is blank
        or malformed.
        """
        line = line.strip()
        if not line or line.startswith("#"):
                return None

        parts = line.split(" ")
        if len(parts) != 3:
                return None

        tags_part, fields_part, ts_part = parts

        tag_kv = tags_part.split(",")[1:]
        tags = {}
        for kv in tag_kv:
                k, _, v = kv.partition("=")
                if k in tags:
                        # Duplicate tag key — a torn write glued two records
                        # together (e.g. "type=acctelemetry,type=sm_pose").
                        return "malformed"
                tags[k] = v
        type_name = tags.get("type")
        if type_name is None:
                return None

        fields = {}
        for kv in fields_part.split(","):
                k, _, v = kv.partition("=")
                if v.endswith("i"):
                        v = v[:-1]
                try:
                        fields[k] = float(v)
                except ValueError:
                        fields[k] = v

        try:
                ts = int(ts_part)
        except ValueError:
                return None

        return type_name, fields, ts


def collect_columns(samples):
        """Walk all samples once to determine the full column set."""
        seen = {}
        for type_name, fields, _ in samples:
                bucket = seen.setdefault(type_name, set())
                bucket.update(fields.keys())

        columns = []
        for type_name in sorted(seen):
                for field in sorted(seen[type_name]):
                        columns.append(f"{type_name}_{field}")
        return columns


def group_samples(samples, window_ns):
        """Yield groups of samples whose timestamps fall within window_ns of
        the first sample in the group.
        """
        group = []
        group_start = None
        for sample in samples:
                ts = sample[2]
                if group_start is None or ts - group_start > window_ns:
                        if group:
                                yield group
                        group = [sample]
                        group_start = ts
                else:
                        group.append(sample)
        if group:
                yield group


def write_csv(samples, columns, window_ns, out):
        writer = csv.writer(out)
        writer.writerow(["timestamp_ns"] + columns)

        for group in group_samples(samples, window_ns):
                row = {col: "" for col in columns}
                for type_name, fields, _ in group:
                        for field, value in fields.items():
                                col = f"{type_name}_{field}"
                                if col in row:
                                        row[col] = value
                writer.writerow([group[0][2]] + [row[c] for c in columns])


def main():
        ap = argparse.ArgumentParser(description=__doc__,
                formatter_class=argparse.RawDescriptionHelpFormatter)
        ap.add_argument("input", type=Path,
                help="Influx line-protocol file (use - for stdin)")
        ap.add_argument("-o", "--output", type=Path, default=None,
                help="Output CSV path (default: stdout)")
        ap.add_argument("--window-ns", type=int, default=1_000_000,
                help="Group window in nanoseconds (default: 1000000 = 1 ms)")
        args = ap.parse_args()

        if str(args.input) == "-":
                src = sys.stdin
        else:
                src = open(args.input, "r")

        samples = []
        malformed = 0
        with src as fh:
                for lineno, line in enumerate(fh, 1):
                        parsed = parse_line(line)
                        if parsed == "malformed":
                                malformed += 1
                                print(f"warning: skipping malformed line "
                                      f"{lineno}: {line.rstrip()}",
                                      file=sys.stderr)
                        elif parsed is not None:
                                samples.append(parsed)
        if malformed:
                print(f"warning: {malformed} malformed line(s) skipped",
                      file=sys.stderr)

        samples.sort(key=lambda s: s[2])
        columns = collect_columns(samples)

        if args.output is None:
                write_csv(samples, columns, args.window_ns, sys.stdout)
        else:
                with open(args.output, "w", newline="") as fh:
                        write_csv(samples, columns, args.window_ns, fh)


if __name__ == "__main__":
        main()
