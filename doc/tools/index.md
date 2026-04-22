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

sim_fetch
sim_flight_kalman
sweep_apogee
sync_defconfig
```

## Overview

| Script | Purpose |
|---|---|
| [`sim_fetch.py`](sim_fetch.md) | Pull files out of a running `native_sim` Zephyr instance via the shell console. |
| [`sim_flight_kalman.py`](sim_flight_kalman.md) | Replay simulated or recorded flights through a Python mirror of the AURORA Kalman filter and plot the result. |
| [`sweep_apogee.py`](sweep_apogee.md) | Grid-search Kalman filter tuning parameters against recorded flights. |
| [`sync_defconfig.py`](sync_defconfig.md) | Merge the generated Zephyr `defconfig` into a board-specific `.conf`. |
