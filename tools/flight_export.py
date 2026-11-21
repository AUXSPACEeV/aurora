#!/usr/bin/env python3
#
# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""CSV export of decoded AURORA flight data.

Takes the ``streams`` dict the rest of the tools pass around
(``name -> (t_s, values[N, K])``, time in float seconds) and writes it
out for spreadsheet analysis.  Three layouts, all keyed on the ``time``
column in seconds:

* :data:`MODE_RESAMPLED` -- one table, equally spaced rows, every column
  linearly interpolated.
* :data:`MODE_RAW` -- one table with every sample exactly as logged;
  cells a sample does not carry are empty.
* :data:`MODE_PER_SENSOR` -- one CSV per stream, zipped.

Column headers are ``<stream>_<field> [<unit>]`` (``accel_x [m/s^2]``,
``baro_pres [kPa]``, ...), with the field order of
:data:`plot_flight_data.FIELD_SPECS`.

This module has no GUI dependency; :func:`export` is the single entry
point the viewer calls.
"""

import io
import re
import zipfile
from collections import namedtuple

import numpy as np

from plot_flight_data import FIELD_SPECS

MODE_RESAMPLED = "resampled"
MODE_RAW = "raw"
MODE_PER_SENSOR = "per_sensor"

#: ``title`` and ``description`` are what the export dialog shows,
#: ``suffix`` tells the exports of one flight apart in the file name.
ExportMode = namedtuple("ExportMode", "title ext suffix description")

MODES = {
    MODE_RESAMPLED: ExportMode(
        "CSV: single table, resampled", ".csv", "resampled",
        "One table with a row every 1/rate seconds. Every sensor is "
        "linearly interpolated onto that grid, so each row holds a value "
        "for every column. Interpolated values are estimates, not "
        "measurements. Best for plotting or calculating in a "
        "spreadsheet."),
    MODE_RAW: ExportMode(
        "CSV: single table, raw samples", ".csv", "raw",
        "One table with every sample exactly as it was logged. A row is "
        "one timestamp; sensors that did not sample at that moment are "
        "left empty. Nothing is interpolated, so this is the data as "
        "recorded."),
    MODE_PER_SENSOR: ExportMode(
        "CSV: one file per sensor (ZIP)", ".zip", "per-sensor",
        "A ZIP with one CSV per sensor (accel.csv, baro.csv, ...), each "
        "at that sensor's own sample times. Nothing is interpolated and "
        "there are no empty cells; sensors with different rates are "
        "kept apart."),
}

#: Measured streams first, then the values computed on the board.
STREAM_ORDER = ("accel", "gyro", "mag", "haccel", "baro", "vbat",
                "sm_kinematics", "sm_pose", "orientation")

#: Streams computed on the board rather than measured.
DERIVED_STREAMS = ("sm_kinematics", "sm_pose", "orientation")

TIME_COLUMN = "time"
TIME_UNIT = "s"

#: Unit of each stream's value columns, as the plots label them.
#: ASCII only, so the header survives any spreadsheet's CSV import.
UNITS = {
    "accel":         ("m/s^2",) * 3,
    "gyro":          ("rad/s",) * 3,
    "mag":           ("gauss",) * 3,
    "haccel":        ("m/s^2",) * 3,
    "baro":          ("kPa", "degC"),
    "vbat":          ("V",),
    "sm_kinematics": ("m/s^2", "m/s^2"),
    "sm_pose":       ("m/s", "m"),
    "orientation":   ("deg",) * 3,
}
#: Significant digits written for sample values (pressure in Pa needs 8+).
VALUE_FORMAT = "{:.9g}"


def column_names(stream):
    """CSV column headers of one stream, ``accel_x [m/s^2]``."""
    return [f"{stream}_{field} [{unit}]"
            for field, unit in zip(FIELD_SPECS[stream], UNITS[stream])]


def time_column():
    return f"{TIME_COLUMN} [{TIME_UNIT}]"


def safe_filename(text, limit=80):
    """``text`` reduced to letters, digits, ``.``, ``-`` and ``_``."""
    name = re.sub(r"[^\w.-]+", "_", text).strip("._-")
    return name[:limit].rstrip("._-") or "flight"


def default_filename(mode, stem, rate_hz=None):
    """Suggested file name: ``<stem>_<mode>[_<rate>Hz]<ext>``."""
    spec = MODES[mode]
    rate = f"_{rate_hz:g}Hz" if mode == MODE_RESAMPLED and rate_hz else ""
    return f"{stem}_{spec.suffix}{rate}{spec.ext}"


def _used_streams(streams):
    """Names of the non-empty streams, in :data:`STREAM_ORDER`."""
    return [name for name in STREAM_ORDER
            if name in streams and streams[name][0].size]


def _fmt(value):
    return "" if np.isnan(value) else VALUE_FORMAT.format(value)


def _write_table(f, header, times, table):
    """Write ``times`` plus the rows of ``table`` (NaN -> empty cell)."""
    f.write(",".join(header) + "\n")
    for t, row in zip(times, table):
        f.write(f"{t:.6f}," + ",".join(_fmt(v) for v in row) + "\n")


def stream_rates(streams):
    """Typical sample rate in Hz of every non-empty stream.

    Taken from the median gap between distinct timestamps, so pauses in
    the log and several samples sharing a timestamp do not skew it.
    Streams with fewer than two distinct timestamps are left out.
    """
    rates = {}
    for name in _used_streams(streams):
        gaps = np.diff(np.unique(streams[name][0]))
        if gaps.size:
            rates[name] = 1.0 / float(np.median(gaps))
    return rates


def suggest_rate_hz(streams):
    """Default resample rate: that of the fastest *measured* stream.

    A finer grid would only repeat interpolated values, and a coarser
    one would throw samples of the fastest sensor away.  The derived
    state machine / orientation streams are ignored here, since they
    follow the measurements.  Rounded to two significant digits so
    ``49.8`` becomes ``50``.
    """
    rates = stream_rates(streams)
    measured = [r for n, r in rates.items() if n not in DERIVED_STREAMS]
    best = max(measured or rates.values(), default=0.0)
    return float(f"{best:.2g}") if best >= 1.0 else 100.0


def describe_rates(streams):
    """One line listing every stream's rate, for the export dialog."""
    rates = stream_rates(streams)
    return ", ".join(f"{n} {r:.3g} Hz" for n, r in rates.items())


def _occurrence(t):
    """Index of each sample among those sharing its timestamp (0, 1, ...)."""
    order = np.argsort(t, kind="stable")
    ts = t[order]
    first = np.r_[True, ts[1:] != ts[:-1]]
    start = np.maximum.accumulate(np.where(first, np.arange(ts.size), 0))
    occ = np.empty(t.size, dtype=np.int64)
    occ[order] = np.arange(ts.size) - start
    return occ


def write_raw(streams, f):
    """Mode 2: every sample as logged, merged on identical timestamps.

    Samples of different streams with the same timestamp share a row.
    Two samples of the *same* stream with the same timestamp get a row
    each, so no logged sample is dropped.
    """
    names = _used_streams(streams)
    if not names:
        raise ValueError("no samples to export")

    keys = [np.column_stack([streams[n][0], _occurrence(streams[n][0])])
            for n in names]
    unique, inverse = np.unique(np.vstack(keys), axis=0,
                                return_inverse=True)
    inverse = inverse.reshape(-1)

    header = [time_column()]
    blocks = []
    pos = 0
    for name, key in zip(names, keys):
        vals = streams[name][1]
        rows = inverse[pos:pos + len(key)]
        pos += len(key)
        block = np.full((len(unique), vals.shape[1]), np.nan)
        block[rows] = vals
        blocks.append(block)
        header += column_names(name)
    _write_table(f, header, unique[:, 0], np.hstack(blocks))


def write_resampled(streams, f, rate_hz):
    """Mode 1: equally spaced rows, values linearly interpolated.

    Outside a stream's own time range the cell stays empty rather than
    holding an extrapolated value.
    """
    if rate_hz <= 0:
        raise ValueError("resample rate must be positive")
    names = _used_streams(streams)
    if not names:
        raise ValueError("no samples to export")
    start = min(float(streams[n][0][0]) for n in names)
    stop = max(float(streams[n][0][-1]) for n in names)
    times = start + np.arange(int((stop - start) * rate_hz) + 1) / rate_hz

    header = [time_column()]
    blocks = []
    for name in names:
        t, vals = streams[name]
        block = np.full((times.size, vals.shape[1]), np.nan)
        for col in range(vals.shape[1]):
            ok = ~np.isnan(vals[:, col])
            if ok.any():
                block[:, col] = np.interp(times, t[ok], vals[ok, col],
                                          left=np.nan, right=np.nan)
        blocks.append(block)
        header += column_names(name)
    _write_table(f, header, times, np.hstack(blocks))


def write_per_sensor_zip(streams, path):
    """Mode 3: one ``<stream>.csv`` per non-empty stream in a ZIP."""
    names = _used_streams(streams)
    if not names:
        raise ValueError("no samples to export")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            t, vals = streams[name]
            buf = io.StringIO()
            _write_table(buf, [time_column()] + column_names(name), t, vals)
            archive.writestr(f"{name}.csv", buf.getvalue())


def export(mode, streams, path, rate_hz=None):
    """Write ``streams`` to ``path`` in the layout selected by ``mode``."""
    if mode == MODE_PER_SENSOR:
        write_per_sensor_zip(streams, path)
        return
    with open(path, "w", newline="") as f:
        if mode == MODE_RESAMPLED:
            write_resampled(streams, f,
                            rate_hz or suggest_rate_hz(streams))
        elif mode == MODE_RAW:
            write_raw(streams, f)
        else:
            raise ValueError(f"unknown export mode: {mode!r}")
