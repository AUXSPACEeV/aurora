# Multimeter Test Flight 2026-04-18

## Info

- AURORA Version: `0.2.1`
- Board: `esp32s3-micrometer/esp32s3/procpu`

## Compile command

```bash
python3 tools/sim_flight_kalman.py --flight flight_logs/2026-04-18 \
  --q-vel 3.0 \
  --r-meas 6.0 \
  --debounce 1 \
  --pre-boost 4 \
  --post-main 4
```

## Flight Protocol

Two flights are recorded in `flights.influx`:

- Flight1:
  - parachute opened too late
  - state machine didn't go from MAIN to REDUNDAND
- Flight2:
  - parachute didn't open; pyro fired on the ground
  - state machine didn't go from MAIN to REDUNDAND

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

### Flight 2

```{image} ./plots/flight2_light.png
:alt: flight2_light.png
:class: only-light
```

```{image} ./plots/flight2_dark.png
:alt: flight2_dark.png
:class: only-dark
```
