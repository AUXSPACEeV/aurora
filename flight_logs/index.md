# Flight Logs

Recorded and simulated flight data lives in
[`aurora/flight_logs`](https://github.com/AUXSPACEeV/aurora/tree/main/flight_logs).
Each directory contains the raw [InfluxDB](https://www.influxdata.com/) line
protocol export (`flights.influx`), a `state_audit` dump and rendered plots
produced by `tools/sim_flight_kalman.py` (see {doc}`/tools/sim_flight_kalman`).

```{note}
Log payloads (`*.influx`, `*.png`, `state_audit`) are tracked via
[Git LFS](https://git-lfs.com/). The documentation build requires
`git lfs pull` (or `actions/checkout` with `lfs: true`) so that image
references in the flight Readme files resolve to real files rather than LFS
pointer blobs.
```

```{toctree}
:maxdepth: 1

2026-04-18/flight_log
2026-04-26/flight_log
sim_fetch/flight_sim
```
