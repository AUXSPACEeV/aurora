# binlog.py

**Source:** [`tools/binlog.py`](https://github.com/AUXSPACEeV/aurora/blob/main/tools/binlog.py)

Host-side reader for the data logger's binary flight logs — the format
described in {ref}`bin-format` and written by the `bin`
formatter on all three backends (flash partition, raw disk region,
filesystem file).

## What it does

The binary log is the lossless record of a flight: it preserves the
`sensor_value` channels exactly, so a host can re-run the filters and
the state machine bit-for-bit. On target it is normally translated to
CSV or InfluxDB line protocol by `data_logger_convert()`. This module
does that translation on the host instead, which matters whenever the
conversion pass never ran — a watchdog reset, a freeze, or a flat pack
between touchdown and conversion all leave a perfectly good `.bin`
behind and nothing else.

It decodes a log into the same `streams` dictionary that
[`plot_flight_data.py`](plot_flight_data.md) builds from
`flights.influx`, so the existing plotting helpers accept it unchanged.
Decoding is vectorised through NumPy: a 4 MiB, 140 000-record log reads
in well under a tenth of a second.

Readers for the converter's `.csv` and `.influx` output are included as
well, so a caller can load anything found in a `DATA` directory through
one interface.

### What it handles

- **Frame-size detection.** `CONFIG_DATA_LOGGER_BIN_FRAME_SIZE` is not
  recorded in the file, so the reader finds it: the frame size is the
  smallest stride that lands on a valid header for every whole frame in
  the file. Records never start with the magic, so the probe cannot be
  fooled by a smaller stride.
- **Torn tails.** A file cut short mid-frame by a reset or a power loss
  is read up to its last whole frame, and the leftover bytes are
  reported as a warning.
- **Segments.** Frames are grouped into runs sharing a `flight_id` with
  a monotonic `seq`. A new segment starts when the `flight_id` changes,
  when the sequence skips, or when the per-frame `base_ts_ns` goes
  backwards — the last of which is the seam a mid-flight reset leaves,
  since the FS backend keeps the `flight_id` but re-derives the
  timestamp base from the restarted uptime clock.
- **Unwritten and unsupported frames** are skipped and counted rather
  than aborting the read.

## Usage

```
python3 tools/binlog.py PATH [--frame-size BYTES] [--csv OUT]
```

- `PATH` — a `.bin` flight log to inspect, or a directory to list.
- `--frame-size BYTES` — override the frame-size probe.
- `--csv OUT` — write the decoded records to a CSV file laid out like
  the on-target converter's output.

Inspecting a log prints the frame and record counts, then one block per
segment:

```console
$ python3 tools/binlog.py DATA/flight_0.bin
DATA/flight_0.bin
  frame size : 4096 B
  frames     : 1106 (0 unwritten/invalid)
  records    : 140400

  segment 1: flight_id=123456789 seq 0..1105 (1106 frames, 140400 records)
    duration : 200.000 s (5000000000 .. 204990000000 ns)
    accel          20000 samples
    baro           20000 samples
    ...
```

## Reuse from other scripts

- `read_binary_log(path, frame_size=None)` — full decode, returning a
  `BinLog` with a `Segment` per run. Each segment carries its own
  `streams` dict plus frame/record counts and `flight_id`.
- `load_streams(path)` — one `streams` dict for any supported log type
  (`.bin`, `.csv`, `.influx`), concatenating segments if there are
  several.
- `scan_data_dir(directory)` — list the flight logs in a `DATA`
  directory, lossless `.bin` first.
- `find_data_dir(directory)` — the `DATA` directory of a card, given
  either it or the card root it sits on.
- `find_audit_dir(directory)` / `scan_audit_dir(directory)` — the
  card's `STATE` directory and the `AUDIT.<n>` dumps in it, in index
  order.
- `scan_audits(path)` — every audit reachable from a log, a `DATA`
  directory or a card root, including a legacy `state_audit` sitting
  next to the logs.
- `find_state_audit(path)` — locate a single `state_audit` dump next to
  a log.
- `detect_frame_size(raw)` — the frame-size probe on its own.
- `STREAM_SPECS` — stream name to (`enum aurora_data` value, record
  channels), the mapping that keeps the decoder aligned with
  `data_logger.h`.

A `streams` value is `(t_ns, values)`: an int64 array of nanosecond
timestamps and an `(N, K)` float array whose columns follow
`plot_flight_data.FIELD_SPECS`.

```{warning}
The constants at the top of this module mirror `data_logger.h`.
Bumping `AURORA_BIN_VERSION` in the firmware means updating
`BIN_VERSION` (and whatever changed) here — the reader refuses frames
whose version it does not know rather than misreading them.
```

## Requirements

- Python 3.10+
- `numpy`
