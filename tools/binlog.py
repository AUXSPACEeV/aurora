#!/usr/bin/env python3
#
# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""Host-side reader for AURORA binary flight logs.

Decodes the fixed-size frame format written by the data logger's binary
backends (``lib/data/fmt_bin.c``, ``fmt_bin_disk.c``, ``fmt_bin_fs.c``)
into the same ``streams`` dictionary that
:func:`plot_flight_data.parse_influx` produces, so recorded ``.bin``
files can be fed straight into the existing plotting helpers without
running the on-target CSV/Influx converter first.

On-storage layout (mirrors ``AURORA_BIN_VERSION`` 2, see
``include/aurora/lib/data_logger.h``)::

    frame  = 32-byte aurora_bin_frame_header + N * 32-byte record
    header = magic[4] "AURF", version u16, reserved u16, seq u32,
             flight_id u64, base_ts_ns u64, reserved u32
    record = type u8, channel_count u8, reserved u16, ts_delta_us u32,
             3 * { val1 i32, val2 i32 }

A record whose ``type`` is ``0xFF`` marks the end of the used part of a
frame (erased flash / 0xFF-filled padding).  All integers are
little-endian.

This module also carries readers for the converter's text outputs
(``.csv`` and ``.influx``) so a whole ``DATA`` directory can be loaded
through one interface.

A card written by a sensor board holds both, side by side::

    /DATA/FLIGHT_0.bin    CONFIG_DATA_LOGGER_BASE_PATH
    /DATA/FLIGHT_0.csv
    /STATE/AUDIT.0        CONFIG_AURORA_STATE_MACHINE_AUDIT_BASE_PATH

:func:`find_data_dir`, :func:`find_audit_dir` and :func:`scan_audits`
locate those from the card root, so a caller can be handed the mount
point instead of the two directories separately.

Run as a script to inspect a log::

    python3 tools/binlog.py DATA/flight_0.bin
    python3 tools/binlog.py --csv out.csv DATA/flight_0.bin
"""

import argparse
import csv
import os
import re
import sys

import numpy as np


# ---------------------------------------------------------------------------
# On-storage format constants (must mirror data_logger.h)
# ---------------------------------------------------------------------------

FRAME_MAGIC = b"AURF"
BIN_VERSION = 2
FRAME_HDR_SIZE = 32
RECORD_SIZE = 32
DP_MAX_CHANNELS = 3
RECORD_END = 0xFF

#: Default ``CONFIG_DATA_LOGGER_BIN_FRAME_SIZE``.
DEFAULT_FRAME_SIZE = 4096

#: Frame sizes probed by :func:`detect_frame_size`, smallest first.
FRAME_SIZE_CANDIDATES = (512, 1024, 2048, 4096, 8192, 16384, 32768,
                         65536)

HDR_DTYPE = np.dtype([
    ("magic", "S4"),
    ("version", "<u2"),
    ("reserved0", "<u2"),
    ("seq", "<u4"),
    ("flight_id", "<u8"),
    ("base_ts_ns", "<u8"),
    ("reserved1", "<u4"),
])

REC_DTYPE = np.dtype([
    ("type", "u1"),
    ("channel_count", "u1"),
    ("reserved", "<u2"),
    ("ts_delta_us", "<u4"),
    ("channels", "<i4", (DP_MAX_CHANNELS, 2)),
])

assert HDR_DTYPE.itemsize == FRAME_HDR_SIZE
assert REC_DTYPE.itemsize == RECORD_SIZE


#: ``enum aurora_data`` -> logger type name (``aurora_data_names``).
TYPE_NAMES = {
    0: "baro",
    1: "accel",
    2: "gyro",
    3: "mag",
    4: "sm_kinematics",
    5: "sm_pose",
    6: "orientation",
    7: "vbat",
}

AURORA_DATA_COUNT = len(TYPE_NAMES)

#: Stream name -> (``enum aurora_data`` value, record channels in the
#: column order used by ``plot_flight_data.FIELD_SPECS``).
#:
#: ``baro`` is the odd one out: the datapoint carries
#: ``[0] temperature, [1] pressure`` while the plotting code (and the
#: Influx/CSV converters) order the columns ``(pres, temp)``.
STREAM_SPECS = {
    "accel":         (1, (0, 1, 2)),
    "gyro":          (2, (0, 1, 2)),
    "mag":           (3, (0, 1, 2)),
    "baro":          (0, (1, 0)),
    "sm_kinematics": (4, (0, 1)),
    "sm_pose":       (5, (0, 1)),
    "orientation":   (6, (0, 1, 2)),
    "vbat":          (7, (0,)),
}


def empty_streams():
    """Return a ``streams`` dict with every known stream empty."""
    return {
        name: (np.empty(0, dtype=np.int64),
               np.empty((0, len(chans)), dtype=float))
        for name, (_type, chans) in STREAM_SPECS.items()
    }


# ---------------------------------------------------------------------------
# Frame decoding
# ---------------------------------------------------------------------------

class BinLogError(Exception):
    """Raised when a file cannot be decoded as an AURORA binary log."""


class Segment:
    """One contiguous run of frames sharing a ``flight_id``.

    A single ``.bin`` file normally holds exactly one segment: the FS
    backend appends every arm of a boot session to the same file and
    keeps the ``flight_id`` stable across them.  A second segment shows
    up when the file was reused across boots, when a reset restarted the
    uptime clock the per-frame ``base_ts_ns`` is derived from, or when
    frames were lost and ``seq`` skipped.
    """

    def __init__(self, flight_id, first_seq, last_seq, first_frame,
                 frame_count, streams, record_count, split_reason=None):
        self.flight_id = flight_id
        self.first_seq = first_seq
        self.last_seq = last_seq
        self.first_frame = first_frame
        self.frame_count = frame_count
        self.streams = streams
        self.record_count = record_count
        #: Why this segment starts where it does (``None`` for the first).
        self.split_reason = split_reason

    @property
    def t_start_ns(self):
        starts = [t[0] for t, _v in self.streams.values() if t.size]
        return int(min(starts)) if starts else 0

    @property
    def t_end_ns(self):
        ends = [t[-1] for t, _v in self.streams.values() if t.size]
        return int(max(ends)) if ends else 0

    @property
    def duration_s(self):
        return (self.t_end_ns - self.t_start_ns) / 1e9

    def stream_counts(self):
        return {name: int(t.size) for name, (t, _v) in self.streams.items()}

    def __repr__(self):
        return (f"<Segment flight_id={self.flight_id} "
                f"seq={self.first_seq}..{self.last_seq} "
                f"frames={self.frame_count} records={self.record_count} "
                f"duration={self.duration_s:.1f}s>")


class BinLog:
    """A decoded binary flight log file."""

    def __init__(self, path, frame_size, segments, warnings,
                 total_frames, bad_frames):
        self.path = path
        self.frame_size = frame_size
        self.segments = segments
        self.warnings = warnings
        self.total_frames = total_frames
        self.bad_frames = bad_frames

    @property
    def record_count(self):
        return sum(s.record_count for s in self.segments)

    def __repr__(self):
        return (f"<BinLog {os.path.basename(self.path)} "
                f"frame_size={self.frame_size} "
                f"segments={len(self.segments)}>")


def _frame_headers(raw, frame_size):
    """View the header of every whole frame in ``raw`` as a record array."""
    n = len(raw) // frame_size
    if n == 0:
        return np.empty(0, dtype=HDR_DTYPE)
    grid = np.frombuffer(raw[:n * frame_size],
                         dtype=np.uint8).reshape(n, frame_size)
    return grid[:, :FRAME_HDR_SIZE].copy().view(HDR_DTYPE).reshape(n)


def detect_frame_size(raw, hint=None):
    """Guess ``CONFIG_DATA_LOGGER_BIN_FRAME_SIZE`` from the file bytes.

    A frame always starts with the magic, and records never do, so the
    true frame size is the smallest stride that lands on a valid header
    for *every* whole frame in the file.  ``hint`` is tried first so a
    caller that knows the build's frame size never pays for the probe.

    Returns ``None`` when no candidate fits (not an AURORA log, or a
    frame size outside :data:`FRAME_SIZE_CANDIDATES`).
    """
    if not raw.startswith(FRAME_MAGIC):
        return None

    candidates = list(FRAME_SIZE_CANDIDATES)
    if hint is not None and hint in candidates:
        candidates.remove(hint)
        candidates.insert(0, hint)

    for size in candidates:
        if size <= FRAME_HDR_SIZE + RECORD_SIZE or len(raw) < size:
            continue
        hdrs = _frame_headers(raw, size)
        if hdrs.size == 0:
            continue
        if np.all(hdrs["magic"] == FRAME_MAGIC):
            return size

    return None


def _decode_records(grid, hdrs, frame_size, keep):
    """Decode the records of the frames selected by ``keep``.

    ``grid`` is the (frames, frame_size) byte view of the file.  Returns
    ``(streams, record_count)``.
    """
    recs_per_frame = (frame_size - FRAME_HDR_SIZE) // RECORD_SIZE
    payload = grid[keep, FRAME_HDR_SIZE:
                   FRAME_HDR_SIZE + recs_per_frame * RECORD_SIZE]
    recs = payload.copy().view(REC_DTYPE).reshape(-1, recs_per_frame)

    types = recs["type"]
    # Everything from the first terminator (or corrupt type) onwards is
    # padding, exactly as convert.c treats it.
    stop = (types == RECORD_END) | (types >= AURORA_DATA_COUNT)
    valid = ~np.logical_or.accumulate(stop, axis=1)

    base_ts = hdrs["base_ts_ns"][keep].astype(np.uint64)
    ts = (base_ts[:, None] +
          recs["ts_delta_us"].astype(np.uint64) * np.uint64(1000))

    types = types[valid]
    ts = ts[valid].astype(np.int64)
    chans = recs["channels"][valid]
    # sensor_value: val1 is the integer part, val2 the millionths, both
    # carrying the sign (see format_sensor_value() in fmt_csv.c).
    vals = chans[..., 0].astype(float) + chans[..., 1].astype(float) * 1e-6

    streams = {}
    for name, (type_id, columns) in STREAM_SPECS.items():
        sel = types == type_id
        streams[name] = (ts[sel], vals[sel][:, list(columns)])

    return streams, int(types.size)


def read_binary_log(path, frame_size=None, split_segments=True):
    """Decode an AURORA binary flight log into :class:`BinLog`.

    :param path: path to the ``.bin`` file.
    :param frame_size: override the frame-size probe.
    :param split_segments: split the file into :class:`Segment` objects
        at ``flight_id`` changes, ``seq`` gaps and uptime resets.  Pass
        ``False`` to decode every valid frame into one segment.
    """
    with open(path, "rb") as f:
        raw = f.read()

    if len(raw) < FRAME_HDR_SIZE:
        raise BinLogError(f"{path}: too short to hold a frame header")
    if not raw.startswith(FRAME_MAGIC):
        raise BinLogError(f"{path}: no {FRAME_MAGIC.decode()} magic at "
                          f"offset 0 — not an AURORA binary flight log")

    warnings = []

    if frame_size is None:
        frame_size = detect_frame_size(raw)
        if frame_size is None:
            frame_size = DEFAULT_FRAME_SIZE
            warnings.append(
                f"could not determine the frame size; assuming "
                f"{DEFAULT_FRAME_SIZE} B "
                f"(CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)")

    n_frames = len(raw) // frame_size
    if n_frames == 0:
        raise BinLogError(f"{path}: shorter than one {frame_size} B frame")
    tail = len(raw) - n_frames * frame_size
    if tail:
        warnings.append(f"ignoring a {tail} B partial frame at the end of "
                        f"the file (cut short by a reset or power loss)")

    grid = np.frombuffer(raw[:n_frames * frame_size],
                         dtype=np.uint8).reshape(n_frames, frame_size)
    hdrs = grid[:, :FRAME_HDR_SIZE].copy().view(HDR_DTYPE).reshape(n_frames)

    good = hdrs["magic"] == FRAME_MAGIC
    version_ok = hdrs["version"] == BIN_VERSION
    bad_version = int(np.count_nonzero(good & ~version_ok))
    if bad_version:
        versions = sorted(set(int(v) for v in
                              hdrs["version"][good & ~version_ok]))
        warnings.append(f"{bad_version} frame(s) with unsupported version "
                        f"{versions} skipped (this tool reads v{BIN_VERSION})")
    good &= version_ok

    bad_frames = int(np.count_nonzero(~good))
    idx = np.flatnonzero(good)
    if idx.size == 0:
        raise BinLogError(f"{path}: no valid v{BIN_VERSION} frames found")

    if bad_frames and idx.size:
        warnings.append(f"{bad_frames} of {n_frames} frames are unwritten or "
                        f"unreadable and were skipped")

    # Split into segments before decoding so each one gets its own streams.
    if split_segments:
        bounds, reasons = _segment_bounds(hdrs, idx)
    else:
        bounds, reasons = [(0, idx.size)], [None]

    segments = []
    for (lo, hi), reason in zip(bounds, reasons):
        sel = idx[lo:hi]
        keep = np.zeros(n_frames, dtype=bool)
        keep[sel] = True
        streams, count = _decode_records(grid, hdrs, frame_size, keep)
        segments.append(Segment(
            flight_id=int(hdrs["flight_id"][sel[0]]),
            first_seq=int(hdrs["seq"][sel[0]]),
            last_seq=int(hdrs["seq"][sel[-1]]),
            first_frame=int(sel[0]),
            frame_count=int(sel.size),
            streams=streams,
            record_count=count,
            split_reason=reason,
        ))

    return BinLog(path=path, frame_size=frame_size, segments=segments,
                  warnings=warnings, total_frames=n_frames,
                  bad_frames=bad_frames)


def _segment_bounds(hdrs, idx):
    """Find segment boundaries among the valid frame indices ``idx``.

    Returns ``(bounds, reasons)`` where ``bounds`` is a list of
    ``(lo, hi)`` slices into ``idx`` and ``reasons`` explains each split.
    """
    flight_id = hdrs["flight_id"][idx]
    seq = hdrs["seq"][idx].astype(np.int64)
    base_ts = hdrs["base_ts_ns"][idx].astype(np.int64)

    bounds = []
    reasons = [None]
    start = 0
    for i in range(1, idx.size):
        reason = None
        if flight_id[i] != flight_id[i - 1]:
            reason = (f"flight_id changed "
                      f"{flight_id[i - 1]} -> {flight_id[i]}")
        elif seq[i] != seq[i - 1] + 1:
            reason = (f"frame seq jumped {seq[i - 1]} -> {seq[i]} "
                      f"({seq[i] - seq[i - 1] - 1} frame(s) missing)")
        elif base_ts[i] < base_ts[i - 1]:
            # The FS backend keeps flight_id across a watchdog or fatal
            # reset but re-derives base_ts_ns from the restarted uptime
            # clock, so time appears to run backwards at the seam.
            reason = (f"uptime clock restarted at frame seq {seq[i]} "
                      f"(reset mid-flight)")
        if reason is not None:
            bounds.append((start, i))
            reasons.append(reason)
            start = i
    bounds.append((start, idx.size))

    return bounds, reasons


# ---------------------------------------------------------------------------
# Converter output readers (same streams dict, different container)
# ---------------------------------------------------------------------------

#: CSV column name -> (stream, column index), matching csv_columns[] in
#: ``lib/data/fmt_csv.c``.
CSV_COLUMNS = {
    "accel_x":                  ("accel", 0),
    "accel_y":                  ("accel", 1),
    "accel_z":                  ("accel", 2),
    "baro_pres":                ("baro", 0),
    "baro_temp":                ("baro", 1),
    "gyro_x":                   ("gyro", 0),
    "gyro_y":                   ("gyro", 1),
    "gyro_z":                   ("gyro", 2),
    "mag_x":                    ("mag", 0),
    "mag_y":                    ("mag", 1),
    "mag_z":                    ("mag", 2),
    "sm_kinematics_accel":      ("sm_kinematics", 0),
    "sm_kinematics_accel_vert": ("sm_kinematics", 1),
    "sm_pose_altitude":         ("sm_pose", 1),
    "sm_pose_orientation":      ("sm_pose", 0),
    "vbat_voltage":             ("vbat", 0),
}


def read_csv_log(path):
    """Parse a converter CSV file into a ``streams`` dict.

    One CSV row is a snapshot across sensors, so a stream only picks up
    a sample from rows where at least one of its columns is filled in.
    """
    rows = {name: ([], []) for name in STREAM_SPECS}

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        cols = [(n, s, c) for n, (s, c) in CSV_COLUMNS.items()
                if reader.fieldnames and n in reader.fieldnames]
        for row in reader:
            try:
                t_ns = int(row["timestamp_ns"])
            except (KeyError, TypeError, ValueError):
                continue
            pending = {}
            for name, stream, col in cols:
                text = row.get(name) or ""
                if text == "":
                    continue
                try:
                    value = float(text)
                except ValueError:
                    continue
                width = len(STREAM_SPECS[stream][1])
                pending.setdefault(stream, [np.nan] * width)[col] = value
            for stream, values in pending.items():
                rows[stream][0].append(t_ns)
                rows[stream][1].append(values)

    out = {}
    for name, (ts, vs) in rows.items():
        width = len(STREAM_SPECS[name][1])
        out[name] = (np.array(ts, dtype=np.int64),
                     np.array(vs, dtype=float).reshape(-1, width))
    return out


def read_influx_log(path):
    """Parse a converter InfluxDB line-protocol file into ``streams``."""
    # plot_flight_data owns the line-protocol parser; keep one copy.
    import plot_flight_data as pfd

    streams = empty_streams()
    streams.update(pfd.parse_influx(path))
    return streams


#: Extensions this module can load, mapped to their reader.
LOADERS = {
    ".bin": None,        # handled by read_binary_log (returns a BinLog)
    ".csv": read_csv_log,
    ".influx": read_influx_log,
}


def load_streams(path):
    """Load any supported log file as a single ``streams`` dict.

    Binary logs with more than one segment are concatenated in file
    order; use :func:`read_binary_log` directly to keep them apart.
    """
    ext = os.path.splitext(path)[1].lower()
    if ext == ".bin":
        log = read_binary_log(path)
        if len(log.segments) == 1:
            return log.segments[0].streams
        merged = {}
        for name in STREAM_SPECS:
            parts = [s.streams[name] for s in log.segments]
            merged[name] = (np.concatenate([t for t, _v in parts]),
                            np.concatenate([v for _t, v in parts]))
        return merged
    reader = LOADERS.get(ext)
    if reader is None:
        raise BinLogError(f"{path}: unsupported log type '{ext}'")
    return reader(path)


# ---------------------------------------------------------------------------
# Directory scanning
# ---------------------------------------------------------------------------

#: Names a state-machine audit dump may have next to the logs.
AUDIT_NAMES = ("state_audit", "state_audit.txt", "STATE_AUDIT",
               "STATE_AUDIT.TXT", "audit", "AUDIT")

#: Directory a card's flight logs live in (``CONFIG_DATA_LOGGER_BASE_PATH``).
DATA_DIR_NAMES = ("DATA", "data")

#: Directory the state machine writes its audits to
#: (``CONFIG_AURORA_STATE_MACHINE_AUDIT_BASE_PATH``).
AUDIT_DIR_NAMES = ("STATE", "state")

#: ``AUDIT.<index>``, the name state_audit.c builds for each boot.
AUDIT_FILE_RE = re.compile(r"^audit[._-](\d+)$", re.IGNORECASE)


def find_state_audit(path):
    """Return a ``state_audit`` file sitting next to ``path``, if any."""
    directory = path if os.path.isdir(path) else os.path.dirname(path)
    for name in AUDIT_NAMES:
        candidate = os.path.join(directory, name)
        if os.path.isfile(candidate):
            return candidate
    return None


def _subdir(root, names):
    """First existing subdirectory of ``root`` named like ``names``."""
    for name in names:
        candidate = os.path.join(root, name)
        if os.path.isdir(candidate):
            return candidate
    return None


def scan_data_dir(directory, extensions=(".bin", ".csv", ".influx")):
    """List the flight logs in a data-logger ``DATA`` directory.

    Returns a list of absolute paths sorted by extension priority (the
    lossless ``.bin`` first) and then by name, so the binary log a
    flight was recorded to is offered before the converter's text
    rendering of the same flight.
    """
    try:
        entries = os.listdir(directory)
    except OSError:
        return []

    order = {ext: i for i, ext in enumerate(extensions)}
    found = []
    for name in entries:
        full = os.path.join(directory, name)
        if not os.path.isfile(full):
            continue
        ext = os.path.splitext(name)[1].lower()
        if ext in order:
            found.append((order[ext], name.lower(), full))

    return [full for _o, _n, full in sorted(found)]


def find_data_dir(directory):
    """Return the directory holding the flight logs of ``directory``.

    Accepts either the ``DATA`` directory itself or the root of a card
    written by a sensor board, which keeps the logs one level down.
    Returns ``None`` when neither holds a log.
    """
    if scan_data_dir(directory):
        return directory
    nested = _subdir(directory, DATA_DIR_NAMES)
    if nested and scan_data_dir(nested):
        return nested
    return None


def find_audit_dir(directory):
    """Return the ``STATE`` directory of a card root, if it has one.

    Also accepts the card's ``DATA`` directory, whose sibling it is.
    """
    audit_dir = _subdir(directory, AUDIT_DIR_NAMES)
    if audit_dir:
        return audit_dir
    parent = os.path.dirname(os.path.abspath(directory))
    if parent and parent != os.path.abspath(directory):
        return _subdir(parent, AUDIT_DIR_NAMES)
    return None


def scan_audit_dir(directory):
    """List the ``AUDIT.<index>`` dumps in a ``STATE`` directory.

    Sorted by index, so the audits come back in the order the boots that
    wrote them happened (until the index wraps at
    ``CONFIG_AURORA_STATE_MACHINE_AUDIT_MAX_FILES``).
    """
    try:
        entries = os.listdir(directory)
    except OSError:
        return []

    found = []
    for name in entries:
        full = os.path.join(directory, name)
        if not os.path.isfile(full):
            continue
        match = AUDIT_FILE_RE.match(name)
        if match:
            found.append((int(match.group(1)), name.lower(), full))

    return [full for _i, _n, full in sorted(found)]


def scan_audits(path):
    """Every state-machine audit reachable from ``path``.

    ``path`` may be a log file, a ``DATA`` directory or a card root: the
    card's ``STATE/AUDIT.<index>`` dumps are returned first (in index
    order), followed by any single ``state_audit`` file sitting next to
    the logs, which is how the host-side ``flight_logs`` folders keep
    theirs.  Nothing here decides *which* audit belongs to a given log —
    one card holds one audit per boot and the logs carry no back
    reference, so that is the caller's (or the operator's) call.
    """
    directory = path if os.path.isdir(path) else os.path.dirname(path)
    directory = os.path.abspath(directory)

    found = []
    audit_dir = find_audit_dir(directory)
    if audit_dir:
        found.extend(scan_audit_dir(audit_dir))
    for name in AUDIT_NAMES:
        candidate = os.path.join(directory, name)
        if os.path.isfile(candidate):
            found.append(candidate)

    # AUDIT_NAMES lists the same name in both cases, which a
    # case-insensitive filesystem happily matches twice; identity comes
    # from the inode rather than from the spelling.
    seen = set()
    unique = []
    for candidate in found:
        try:
            info = os.stat(candidate)
            key = (info.st_dev, info.st_ino)
        except OSError:
            key = os.path.normcase(os.path.realpath(candidate))
        if key not in seen:
            seen.add(key)
            unique.append(candidate)
    return unique


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _write_csv(streams, out_path):
    """Write ``streams`` back out in the converter's CSV column layout."""
    names = list(CSV_COLUMNS)
    rows = []
    for stream, (t_ns, vals) in streams.items():
        for i in range(t_ns.size):
            rows.append((int(t_ns[i]), stream, vals[i]))
    rows.sort(key=lambda r: r[0])

    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ns"] + names)
        for t_ns, stream, vals in rows:
            cells = [""] * len(names)
            for i, name in enumerate(names):
                target, col = CSV_COLUMNS[name]
                if target == stream and col < len(vals):
                    cells[i] = f"{vals[col]:.6f}"
            writer.writerow([t_ns] + cells)


def main():
    parser = argparse.ArgumentParser(
        description="Inspect or convert an AURORA binary flight log.")
    parser.add_argument("path", help="path to a .bin flight log or a DATA "
                                     "directory to list")
    parser.add_argument("--frame-size", type=int, default=None,
                        help="override the frame-size probe "
                             "(CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)")
    parser.add_argument("--csv", metavar="OUT",
                        help="write the decoded records to a CSV file in "
                             "the converter's column layout")
    args = parser.parse_args()

    if os.path.isdir(args.path):
        # Accepts the DATA directory or the card root it sits on.
        data_dir = find_data_dir(args.path)
        logs = scan_data_dir(data_dir) if data_dir else []
        if not logs:
            print(f"no flight logs found in {args.path}")
            return 1
        print(f"{len(logs)} log(s) in {data_dir}:")
        for path in logs:
            size = os.path.getsize(path)
            print(f"  {os.path.basename(path):<24} {size / 1024:>10.1f} KiB")
        return 0

    log = read_binary_log(args.path, frame_size=args.frame_size)
    print(f"{args.path}")
    print(f"  frame size : {log.frame_size} B")
    print(f"  frames     : {log.total_frames} "
          f"({log.bad_frames} unwritten/invalid)")
    print(f"  records    : {log.record_count}")
    for warning in log.warnings:
        print(f"  warning    : {warning}")

    for i, seg in enumerate(log.segments, start=1):
        print(f"\n  segment {i}: flight_id={seg.flight_id} "
              f"seq {seg.first_seq}..{seg.last_seq} "
              f"({seg.frame_count} frames, {seg.record_count} records)")
        if seg.split_reason:
            print(f"    split    : {seg.split_reason}")
        print(f"    duration : {seg.duration_s:.3f} s "
              f"({seg.t_start_ns} .. {seg.t_end_ns} ns)")
        for name, count in seg.stream_counts().items():
            if count:
                print(f"    {name:<14} {count} samples")

    if args.csv:
        streams = load_streams(args.path)
        _write_csv(streams, args.csv)
        print(f"\nwrote {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
