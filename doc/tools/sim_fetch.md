# sim_fetch.py

Extract a file from a running `native_sim` Zephyr instance.

## Background

The Zephyr FUSE bridge (`CONFIG_FUSE_FS_ACCESS`) mounts the simulator's
filesystem on the host, but opens files write-only, so `cp` from the FUSE
mount fails with `EIO`. `sim_fetch.py` works around this by dumping the file
over the shell console using `fs read <path>` and reconstructing the bytes
from the hex dump on the host.

The hex dump format emitted by `subsys/fs/shell.c cmd_read` is:

```
File size: <n>
<8 hex digits offset>  <up to 16 "NN " bytes>   <ascii gutter>
...
```

## Modes

The script exposes two subcommands:

| Mode | Description |
|---|---|
| `parse` | Parse a previously captured console log and write the bytes of one `fs read` invocation to disk. |
| `pexpect` | Spawn a `native_sim` binary, drive the shell, and fetch one or more files in a single step. Requires the `pexpect` Python package. |

### `parse`

```
python3 tools/sim_fetch.py parse -o OUTPUT [-i INPUT] [--expect-size N]
```

- `-i / --input` — path to captured console log (`-` for stdin, default).
- `-o / --output` — destination file on the host.
- `--expect-size` — fail unless the dump header reports this size.

The last complete dump in the input is chosen; partial or interrupted dumps
are skipped.

### `pexpect`

```
python3 tools/sim_fetch.py pexpect <zephyr.exe> \
    --fetch REMOTE LOCAL [--fetch REMOTE LOCAL ...] \
    [--await-ready PATTERN] [--pre-command CMD] [--wait-for PATTERN] \
    [--prompt PROMPT] [--timeout S] [--await-timeout S] \
    [--wait-timeout S] [--fetch-timeout S] [-v]
```

Key options:

- `--fetch REMOTE LOCAL` — repeatable; `REMOTE` is the Zephyr path (e.g.
  `/RAM:/data/flight_0.influx`), `LOCAL` is the host destination.
- `--await-ready PATTERN` — block until this regex is seen after boot before
  sending any `--pre-command`. Useful for asynchronous startup such as
  attitude calibration.
- `--pre-command CMD` — shell command(s) to send before fetching (repeatable).
- `--wait-for PATTERN` — after each `--pre-command`, wait for this regex
  instead of the shell prompt (e.g. for a simulator that keeps streaming).
- The `--*-timeout` options control each phase independently.

After all fetches the script issues `kernel reboot cold` and closes the child.

## Example

After building the sensor board for the simulator
(`west build -p -b native_sim sensor_board`):

```bash
python3 ./tools/sim_fetch.py pexpect \
    build/zephyr/zephyr.exe \
    --await-ready "Attitude calibrated" \
    --pre-command "sim launch" \
    --wait-for "LANDED" \
    --wait-timeout 600 \
    --fetch /RAM:/data/flight_0.influx flight_logs/sim_fetch/flights.influx \
    --fetch /RAM:/state/audit.0       flight_logs/sim_fetch/state_audit \
    -v
```

## Requirements

- Python 3.10+
- `pexpect` (only for the `pexpect` mode): `pip install pexpect`
