# Tools

The [`aurora/tools`](https://github.com/AUXSPACEeV/aurora/tree/main/tools)
directory collects standalone Python scripts used during development, testing
and log analysis. They are not part of the firmware build and are invoked
directly with `python3`.

All scripts share the same copyright header and SPDX identifier as the rest
of the repository and target Python 3.10+.

```{note}
Pip requirements are installed via `pip install -r tools/requirements.txt`
```

```{toctree}
:maxdepth: 1

gen_flight_replay
plot_flight_data
sim_fetch
sim_flight_kalman
sweep_apogee
sync_defconfig
```

## Overview

| Script | Purpose |
|---|---|
| [`gen_flight_replay.py`](gen_flight_replay.md) | Convert a recorded `flights.csv` into a generated C source file consumed by the `fake_sensors` replay backend. |
| [`plot_flight_data.py`](plot_flight_data.md) | Plot recorded or simulated flight data — standalone CLI for telemetry logs, importable plotting module for `sim_flight_kalman`. |
| [`sim_fetch.py`](sim_fetch.md) | Pull files out of a running `native_sim` Zephyr instance via the shell console. |
| [`sim_flight_kalman.py`](sim_flight_kalman.md) | Run a synthetic flight through a Python mirror of the AURORA Kalman filter and attitude tracker. |
| [`sweep_apogee.py`](sweep_apogee.md) | Grid-search Kalman filter tuning parameters against recorded flights. |
| [`sync_defconfig.py`](sync_defconfig.md) | Merge the generated Zephyr `defconfig` into a board-specific `.conf`. |
