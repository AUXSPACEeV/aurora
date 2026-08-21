# flight_log_gui.py

**Source:** [`tools/flight_log_gui.py`](https://github.com/AUXSPACEeV/aurora/blob/main/tools/flight_log_gui.py)

Desktop viewer for recorded AURORA flight logs — open a `.bin` straight
off the SD card (or the whole card, `DATA` and `STATE` together) and
page through the flight's graphs without running a command line.

```{note}
The window is a front end, not a second implementation.
[`binlog.py`](binlog.md) decodes the log and
[`plot_flight_data.py`](plot_flight_data.md) draws every panel, so the
plots the GUI shows are the plots the CLI writes.
```

## What it does

- **Reads the binary flight log directly.** The data logger's `bin`
  formatter writes a raw frame stream ({ref}`bin-format`),
  normally turned into CSV/Influx by the on-target conversion pass.
  This tool decodes those frames on the host instead, so a card pulled
  after a watchdog reset — before conversion ever ran — is still
  readable.
- **Browses a whole card.** Point it at the card root or at the data
  logger directory on it — `CONFIG_DATA_LOGGER_BASE_PATH` (`/DATA`) is
  found either way — and every log is listed with its size:
  `FLIGHT_N.bin` first, then the converter's `FLIGHT_N.csv` and
  `FLIGHT_N.influx`. Selecting one decodes it and lists the flights it
  holds.
- **Lists the card's state audits.** The state machine writes one
  `AUDIT.<n>` per boot to `CONFIG_AURORA_STATE_MACHINE_AUDIT_BASE_PATH`
  (`/STATE`), and nothing in a flight log points back at the audit that
  was running while it was written. So the sidebar lists them all, each
  with the number of flights and the timespan it covers, and the
  pairing is the operator's to make.
- **Finds the flight window.** Binary logs record sensor data only, so
  there is no `ARMED → BOOST` transition in the file. The audit picked
  in the sidebar supplies one when it has one; otherwise the boost and
  touchdown instants are recovered from the measurements using the same
  heuristics the CLI falls back to, and the status bar and plot title
  say which signal they came from.
- **Splits a log into segments.** One `.bin` is one boot session, but a
  file can still hold several runs: a change of `flight_id`, a gap in
  the frame sequence, or an uptime clock that restarted mid-file (a
  watchdog or fatal-error reset) each start a new segment, listed
  separately under the file.
- **Light and dark**, both matched to the Furo docs palette
  (`doc/conf.py`) — widgets and plots switch together.

## Usage

```
python3 tools/flight_log_gui.py [PATH]
                                [--theme {light,dark}]
                                [--frame-size BYTES]
```

- `PATH` — a flight log (`.bin`, `.csv`, `.influx`), a `DATA`
  directory or a card root to open on start-up. Optional; the window's
  **Open Log…** and **Open Card…** buttons do the same thing.
- `--theme {light,dark}` — initial colour theme (default `light`).
  Toggle it at any time with the button in the header.
- `--frame-size BYTES` — override the frame-size probe. Only needed
  for a build whose `CONFIG_DATA_LOGGER_BIN_FRAME_SIZE` is not one of
  the sizes [`binlog.py`](binlog.md) probes.

## The window

| Area | What it holds |
|---|---|
| **Flight logs** | Tree of the opened card. Selecting a file decodes it and hangs its flights underneath. |
| **State audit** | The audits reachable from the card — `STATE/AUDIT.<n>`, or a single `state_audit` next to the logs — plus two standing choices. *Automatic* applies the audit whose transitions fall inside the log's timeline (the row says which one won) and *None* always recovers the window from the measurements. Picking one re-cuts the log without decoding it again. |
| **Graphs** | The panels this flight has data for. Multi-select (ctrl/shift-click) to stack a subset; **All** / **None** are shortcuts. |
| **Window** | *Flight window* trims to the detected flight plus the pre-boost / post-end margins; *Whole log* plots the recording end to end. `Kalman R` feeds the NIS-gate replay and should match `CONFIG_FILTER_R_MILLISCALE / 1000`. |
| **Summary** | Frame and record counts, `flight_id`, peak altitude and velocity, per-stream sample counts, and any decode warnings. |
| **Plot** | The stacked panels, scrollable, with the matplotlib pan/zoom toolbar underneath. **Export PNG…** writes the current view at 150 dpi (PDF and SVG also work). |

Panels are only offered for streams the log actually contains, so the
list is shorter for a `.csv` (the firmware's CSV formatter has no
orientation column) than for the `.bin` it came from.

```{note}
An audit and a log line up only when they come from the same boot: both
timestamp against the uptime clock, which restarts at zero every time
the board does. That is what *Automatic* matches on, and why forcing an
audit from another boot puts the flight window somewhere the log has no
samples — the plot area then says so instead of drawing an empty
window.
```

## Requirements

- Python 3.10+
- `numpy`, `matplotlib`, `pillow`
- `tkinter` — part of the standard library, but packaged separately on
  some distributions (`python3-tkinter` on Fedora, `python3-tk` on
  Debian/Ubuntu).

## See also

- [`binlog.py`](binlog.md) — the decoder, also usable from the command
  line and importable from other scripts.
- [`plot_flight_data.py`](plot_flight_data.md) — the plotting module,
  and the CLI for logs that already have `flights.influx` and
  `state_audit`.
