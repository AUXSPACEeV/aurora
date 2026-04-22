# Flight Simulation

## Info

- AURORA Version: `0.2.1`
- Board: `native_sim`

## Compile command

```bash
python3 ./tools/sim_flight_kalman.py --flight flight_logs/sim_fetch \
  --q-vel 3.0 \
  --r-meas 6.0 \
  --debounce 3
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
