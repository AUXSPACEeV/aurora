# M.E.T.A. Test Flight 2026-04-26

## Info

- AURORA Version: `0.3.0`
- Board: `esp32s3-micrometer/esp32s3/procpu`

## Compile command

```bash
# Flight 1
python3 tools/plot_flight_data.py --flight flight_logs/2026-04-26/flight1 \
  --pre-boost 4 \
  --title "AURORA Flight 1 - 2026-04-26"

# Flight 2
python3 tools/plot_flight_data.py --flight flight_logs/2026-04-26/flight2 \
  --pre-boost 4 \
  --title "AURORA Flight 2 - 2026-04-26"
```

## Flight Protocol

Two flights are recorded in `flight1` and `flight2`:

- Flight 1
  - parachute opened as expected
  - state machine cycled as expected
- Flight 2
  - parachute opened as expected
  - state machine cycled as expected

````{note}
The velocity graph is a flat line, not because the filter didn't work, but
because the logger had an issue with a pointer variable that wasn't accessed
correctly.
This issue has been fixed in `f788cd50cffa705b049117e93fb963d218b2784d`
````

````{note}
Voting was also disabled in the graphs, since the faulty data would have
shown the votes as always triggered.
````

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

```{image} ./flight2/plots/flight2_light.png
:alt: flight1_light.png
:class: only-light
```

```{image} ./flight2/plots/flight2_dark.png
:alt: flight1_dark.png
:class: only-dark
```
