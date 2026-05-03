# M.E.T.A. Test Flight 2026-05-03

## Info

- AURORA Version: `0.3.0`
- Board: `esp32s3-micrometer/esp32s3/procpu`

## Compile command

```bash
# Flight 1
python3 tools/sim_flight_kalman.py --flight flight_logs/2026-05-03/flight1 \
  --title "M.E.T.A. Flight 1 - 2026-05-03"

# Flight 2
python3 tools/sim_flight_kalman.py --flight flight_logs/2026-05-03/flight2 \
  --title "M.E.T.A. Flight 2 - 2026-05-03"
```

## Flight Protocol

Two flights are recorded in `flight1` and `flight2`:

- Flight 1
  - parachute opened as expected
  - state machine cycled as expected
- Flight 2
  - parachute failed to open
  - state machine didn't go past `ARMED`
  - IMU Data is missing after ~500 seconds

## Results

### Flight1

```{image} ./flight1/plots/flight1_light.png
:alt: flight1_light.png
:class: only-light
```

```{image} ./flight1/plots/flight1_dark.png
:alt: flight1_dark.png
:class: only-dark
```

### Flight2

````{warning}
This flight had an error, where the rocket launched before attitude
calibration succeeded. This results in the state machine not detecting the
`BOOST` phase.
There is currently no explanation on why the IMU has stopped producing data.
````

```{image} ./flight2/plots/flight2_light.png
:alt: flight2_light.png
:class: only-light
```

```{image} ./flight2/plots/flight2_dark.png
:alt: flight2_dark.png
:class: only-dark
```

