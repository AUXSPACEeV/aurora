# Flight Simulation

## Info

- AURORA Version: `0.2.1`
- Board: `native_sim`

## Compile command

```bash
# Building and running the simulator first:
west build -p -b native_sim/native sensor_board/ -- \
  -DCONFIG_AURORA_FAKE_SENSORS_REPLAY=y

python3 ./tools/sim_fetch.py pexpect \
  build/zephyr/zephyr.exe \
  --await-ready "Attitude calibrated" \
  --pre-command "sim launch" \
  --wait-for "converted flight_log" \
  --wait-timeout 600 \
  --fetch /RAM:/data/flight_0.influx flights.influx \
  --fetch /RAM:/state/audit.0 state_audit \
  --fetch /RAM:/data/flight_0.csv flights.csv \
  -v
```

```bash
python3 ./tools/plot_flight_data.py \
  --flight flight_logs/sim_fetch \
  --title "Simulated Flight (v0.4.1)"
```

## Flight Protocol

One flight is recorded in `flights.influx`:

- parachute opened fine
- state machine behaved as expected

## Results

### Flight 1

```{image} ./plots/flight1_light.png
:alt: flight1_light.png
:class: only-light
```

```{image} ./plots/flight1_dark.png
:alt: flight1_dark.png
:class: only-dark
```
