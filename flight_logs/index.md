# Flight Logs

Recorded and simulated flight data lives in
[`aurora/flight_logs`](https://github.com/AUXSPACEeV/aurora/tree/main/flight_logs).
Each directory contains the raw [InfluxDB](https://www.influxdata.com/) line
protocol export (`flights.influx`), a `state_audit` dump and rendered plots
produced by `tools/plot_flight_data.py` (see {doc}`/tools/plot_flight_data`).

```{note}
Log payloads (`*.influx`, `*.png`, `state_audit`) are tracked via
[Git LFS](https://git-lfs.com/). The documentation build requires
`git lfs pull` (or `actions/checkout` with `lfs: true`) so that image
references in the flight Readme files resolve to real files rather than LFS
pointer blobs.
```

```{toctree}
:maxdepth: 1

multimeter/flight_logs
sim_fetch/flight_sim
```
