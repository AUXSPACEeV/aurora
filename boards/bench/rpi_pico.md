# Raspberry Pi Pico (bench targets)

The [Raspberry Pi Pico](https://www.raspberrypi.com/products/raspberry-pi-pico/)
and [Pico 2](https://www.raspberrypi.com/products/raspberry-pi-pico-2/)
families are supported as **bench / development targets**, not as
flight boards. They are intended for testing the AURORA telemetry
stack and other subsystems without populating a full sensor_board or
micrometer PCB.

The same HC-12 wiring and the same AURORA Kconfig set work across all
of the supported Pico variants. Pick the one whose silicon you want
to validate against.

## Supported variants

| Upstream board target | SoC | Core | Notes |
|---|---|---|---|
| `rpi_pico/rp2040/w`              | RP2040  | Cortex-M0+ | Same RP2040 silicon as the sensor_board v2 (Pico variant). |
| `rpi_pico2/rp2350a/m33/w`        | RP2350A | Cortex-M33 | RP2350A Arm core. |
| `rpi_pico2/rp2350a/hazard3/w`    | RP2350A | Hazard3 (RISC-V) | Same RP2350A silicon, RISC-V core selected. |

The non-W (no Wi-Fi) versions of each target also work. The bench
overlay does not touch the Wi-Fi peripheral, but the `_w` variants
are what we keep on the shelf.

## What this target provides

The board overlays and `.conf` files AURORA layers on top of the
upstream boards live next to the application.

They configure:

- **Console / shell** on USB CDC (`zephyr,console = &cdc_acm_uart`).
- **HC-12 telemetry radio** on UART1: TX=GP4, RX=GP5, SET=GP3
  (active-low, used by the `telemetry hc12` shell to enter AT mode).
- The AURORA telemetry stack (`CONFIG_AURORA_TELEMETRY=y`) with the
  HC-12 shell command (`CONFIG_AURORA_TELEMETRY_HC12_SHELL=y`).
- Floating-point support for `printk`/log formatting
  (`CONFIG_CBPRINTF_FP_SUPPORT=y`), since telemetry payloads carry
  doubles.

![rpi bench](/img/sensor_board_bench.drawio.svg)

No real IMU, barometer, or SD card is wired up. The bench target is
meant to be paired with the `sim` snippet (fake sensors driving the
state machine from a scripted profile) and the `nodisk` snippet (no
flight data logger).

```{note}
Zephyr resolves board overlays and confs by exact board id, so each
buildable target needs its own file pair. There is no implicit
fall-back from `rpi_pico2/rp2350a/m33/w` to a generic `rpi_pico2`
overlay. The Kconfig bodies are kept identical across the bench
variants on purpose; the only per-target differences are the pinctrl
header include and the pinmux group in the overlay.
```

## Build

Pick the line that matches your hardware:

```bash
# Pico W (RP2040, Cortex-M0+)
west build -p -b rpi_pico/rp2040/w -S sim -S nodisk sensor_board

# Pico 2 W (RP2350A, Cortex-M33)
west build -p -b rpi_pico2/rp2350a/m33/w -S sim -S nodisk sensor_board

# Pico 2 W (RP2350A, Hazard3 RISC-V)
west build -p -b rpi_pico2/rp2350a/hazard3/w -S sim -S nodisk sensor_board
```

What each piece does:

- `-p`: pristine build; discard any previous build directory so
  Kconfig values from another target cannot bleed in.
- `-b <board>`: the upstream Zephyr board target.
- `-S sim`: applies the `sim` snippet, swapping real sensor drivers
  for the `fake_sensors` backend.
- `-S nodisk`: applies the `nodisk` snippet, disabling the flight
  data logger and its SD card requirement.
- `sensor_board`: the AURORA application under
  [aurora/sensor_board/](/applications/sensor_board.md) being built.

Build output: `build/zephyr/zephyr.uf2`.

## Flashing

Hold the BOOTSEL button on the board while plugging it in over USB,
then drag-and-drop `build/zephyr/zephyr.uf2` onto the mass-storage
device that appears. The board will reboot into the freshly flashed
firmware.

## Hardware wiring

| Pico pin | Peripheral | Notes |
|---|---|---|
| GP3  | HC-12 SET  | Active-low; lets the shell switch the module into AT mode |
| GP4  | HC-12 RXD  | UART1 TX from the Pico |
| GP5  | HC-12 TXD  | UART1 RX into the Pico |
| 3V3  | HC-12 VCC  | The HC-12 is 3.3 V tolerant |
| GND  | HC-12 GND  |  |

The console and `telemetry hc12` shell are reached over the Pico's USB
CDC interface. No extra USB-to-serial adapter is needed.

## Related

- The matching ground-station receiver:
  [rec_zephyr.py](/tools/rec_zephyr.md).
