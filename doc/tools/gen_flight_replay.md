# gen_flight_replay.py

Convert a recorded AURORA flight CSV into a generated C source file that
embeds the samples as constant arrays. The generated file is compiled into
the firmware and consumed by the **replay backend** of `fake_sensors`, so
that simulated runs (e.g. on `native_sim` or with the `sim` snippet) can be
driven by *real* flight data instead of the analytic profile.

## Background

When `CONFIG_AURORA_FAKE_SENSORS=y` is set, the IMU and baro polling
threads are replaced by a synthetic data source. There are two backends:

| Backend | Kconfig | What it publishes |
|---|---|---|
| Analytic profile | `AURORA_FAKE_SENSORS_SYNTH` | An ISA-troposphere ascent/descent curve generated on the fly. |
| Replay | `AURORA_FAKE_SENSORS_REPLAY` | Samples from a previously recorded flight, embedded at build time. |

The replay backend (`fake_sensors_replay.c`) expects three arrays:

```c
struct replay_imu_sample  { uint64_t t_ns; float x, y, z; };
struct replay_baro_sample { uint64_t t_ns; float pres_kpa, temp_c; };

const struct replay_imu_sample  replay_accel[];
const struct replay_imu_sample  replay_gyro[];
const struct replay_baro_sample replay_baro[];
```

`gen_flight_replay.py` is the small code-generator that produces the C file
holding these arrays. It is invoked automatically from the sensor_board
`CMakeLists.txt` whenever the replay backend is enabled — you normally do
not call it by hand.

## Inputs

The script reads the **`flights.csv`** file produced by the data logger's
CSV converter. Each row is expected to carry a `timestamp_ns` column plus
optional `accel_{x,y,z}`, `gyro_{x,y,z}` and `baro_pres` / `baro_temp`
columns. Empty cells are simply skipped, which is convenient because the
logger only fills the columns of the sensor that produced a given row.

Optionally, a **`state_audit`** file from the same flight may be passed in.
This is the human-readable transition log written by the flight state
machine. The script parses the `BOOST` and `LANDED` transitions out of it
and trims the sample arrays to a window of:

```
[BOOST  - 4 s, LANDED + 4 s]
```

Trimming keeps the embedded array small (and therefore the firmware image
small) while still leaving enough margin on either side for attitude
calibration and post-landing checks.

```{note}
Despite the `Time (ms)` header in the audit file, the recorded values are
actually in nanoseconds and match `timestamp_ns` in the CSV one-to-one.
```

## Output

A single C source file with:

- A header comment listing the source CSV, the sample counts and the
  total time span (and, if applicable, the trim window).
- Three array definitions: `replay_accel`, `replay_gyro`, `replay_baro`.
- A `<name>_len` constant for each array (computed via `ARRAY_SIZE`).

All timestamps in the generated file are **rebased to start at zero** —
the smallest timestamp across all three streams is subtracted from every
sample. This way the replay engine can work in elapsed time without
caring about the absolute uptime of the original flight.

## Usage

```
python3 tools/gen_flight_replay.py \
    --input        flights.csv \
    --output       replay_data.c \
    [--state-audit state_audit]
```

| Argument | Description |
|---|---|
| `--input` | Path to a `flights.csv` produced by the data logger. |
| `--output` | Destination path for the generated C source file. |
| `--state-audit` | Optional. State machine audit log from the *same* flight. When present, samples are trimmed to `[BOOST - 4 s, LANDED + 4 s]`. |

The script exits with a non-zero status (and a message on `stderr`) if:

- the CSV is missing accelerometer, gyroscope or barometer samples, or
- a `--state-audit` is supplied but contains no `BOOST` / `LANDED`
  transition, or
- the trim window ends up empty (typically a sign that the audit and CSV
  timestamps come from different runs).

## Build integration

You usually do not run `gen_flight_replay.py` directly. The
sensor_board `CMakeLists.txt` adds a custom command that re-runs it
whenever the input CSV (or, if present, the sibling `state_audit` file)
changes:

```cmake
add_custom_command(
    OUTPUT  ${REPLAY_OUT}
    COMMAND ${PYTHON_EXECUTABLE} ${REPLAY_GEN}
        --input ${REPLAY_INPUT}
        --output ${REPLAY_OUT}
        ${REPLAY_AUDIT_ARGS}
    DEPENDS ${REPLAY_INPUT} ${REPLAY_GEN} ${REPLAY_AUDIT_DEPS}
    COMMENT "Generating replay_data.c from ${REPLAY_INPUT_REL}"
)
```

To swap recordings, change the Kconfig string
`CONFIG_AURORA_FAKE_SENSORS_REPLAY_INPUT` (path is relative to the aurora
module root). If a `state_audit` file sits next to the chosen CSV, it is
picked up automatically.

## Example

Generate a replay source file from a recorded multimeter flight and trim
it to the powered-flight window:

```bash
python3 ./tools/gen_flight_replay.py \
    --input        flight_logs/multimeter/2026-05-03/flight1/flights.csv \
    --output       /tmp/replay_data.c \
    --state-audit  flight_logs/multimeter/2026-05-03/flight1/state_audit
```

To then build a `native_sim` image that replays this flight:

```bash
west build -p -b native_sim -S sim aurora/sensor_board \
    -- -DCONFIG_AURORA_FAKE_SENSORS_REPLAY=y
./build/zephyr/zephyr.exe
# in the Zephyr shell:
uart:~$ sim launch
```

## Requirements

- Python 3.10+
- No third-party packages (uses `argparse`, `csv` and `re` only).
