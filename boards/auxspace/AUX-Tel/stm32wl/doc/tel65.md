```{zephyr:board} tel65
```

# Overview

The Auxspace e.V. AUX-Tel 65 PCB is the telemetry board of the
**AUX-Stack**. It is a 65 mm round board that stacks on top of (or below)
{doc}`AUX-Core </boards/auxspace/AUX-Core/esp32s3/doc/core65>` through the
same pair of AUX-Stack connectors, and carries the stack's long-range
downlink.

It is built around a
[Daphnis-I](https://www.we-online.com/en/components/products/DAPHNIS-I)
LoRa module (order code 2618011181000) from
[Würth Elektronik](https://www.we-online.com/), which packages an
[STM32WLE5CCU6](https://www.st.com/en/microcontrollers-microprocessors/stm32wle5cc.html)
(256 kB flash, 64 kB SRAM) together with its 32 MHz TCXO, LSE crystal and
RF matching network behind a single 50 Ω port, brought out as both an
RF pad and a UMRF connector.

More about the SoC itself can be found in the Zephyr docs of the
[STM32WL](https://docs.zephyrproject.org/latest/boards/st/nucleo_wl55jc/doc/index.html).

```{warning}
AUX-Tel is still under hardware bring-up. This board definition is synced
against the AUX-Tel repo at `e71a667` (2026-11-03), still rev 01. One of
the two netlist changes it originally assumed has since been cut; the
other has not, and the MCP2515 cannot be used without it. See
[Outstanding rev-01 fixes](#outstanding-rev-01-fixes) before building
hardware against it.
```

```{note}
Daphnis-I (2618011181000) and
[Oceanus-I](https://www.we-online.com/en/components/products/OCEANUS-I)
(2618011182000) are the same module hardware. Their datasheets give an
identical pad map pin for pin, an identical 15.0 x 16.0 x 3.0 mm outline
and land pattern, the same STM32WLE5CCU6 and the same 863..870 MHz band;
they differ only in that Oceanus-I ships blank while Daphnis-I ships with
Würth's LoRaWAN / WE-ProWare AT firmware.

That is why `boards/auxspace/AUX-Tel/daphnis1.dtsi` takes the RF
front-end pins from `we_oceanus1ev`
(`zephyr/dts/arm/we/oceanus1.dtsi`), which is the upstream twin of that
file - diff against it when bumping Zephyr. The two switch GPIOs are
still the one detail neither datasheet spells out, since they are
internal to the shield, and a wrong switch pin fails silently: the radio
boots clean and hears nothing. Check them first if RF ever measures deaf.
```

# Hardware

- Würth Daphnis-I (STM32WLE5CCU6) LoRa module. The antenna attaches to
  the module's UMRF (U.FL / MHF1) connector; the RF pad is deliberately
  left as an unconnected solder pad, which is what the user manual
  requires when the UMRF connector is used.
- Two identical AUX-Stack data connectors (top and bottom) for stacking,
  pin-compatible with AUX-Core. See
  [Connections and IOs](#connections-and-ios) below.
- Microchip MCP2515 SPI CAN controller (8 MHz crystal) behind a TI TCAN334
  transceiver, tapped off the stack CAN pair through R21/R22, with split
  termination on board (2 x 60R4 = 120.8 Ω). Disabled in devicetree by
  default. Its interrupt line is still unrouted, see
  [Outstanding rev-01 fixes](#outstanding-rev-01-fixes).
- Inter-board link to AUX-Core over the stack UART (LPUART1)
- J5 2x4 2.54 mm debug header: SWDIO, SWDCLK, the USART1 console, and
  +3V3 / GND
- Two green status LEDs (D2, D3) on the module's `LED_2` / `LED_1` pins,
  plus D1 as a power-on indicator
- RESET (SW4) and BOOT (SW3) buttons
- TPS7B8601 3V3 LDO fed by a jumper-selected VBus rail (JP3..JP7)

## Supported Features

```{zephyr:board-supported-hw}
```

## Connections and IOs

A detailed physical pinout diagram is not yet available, since the PCB is
under active development.

### Daphnis-I pin usage

| Daphnis pin | STM32 | AUX-Tel net |
|---|---|---|
| 1 `GPIO_0` | PB7 | `LIFTOFF` |
| 2 `GPIO_1` | PB8 | `ARM` |
| 4 `/RESET` | NRST | SW4 to GND, 11k pull-up to +3V3 |
| 5 `LPUTXD1` | PA2 | stack `UART_RX` |
| 6 `LPURXD1` | PA3 | stack `UART_TX` |
| 7 `LED_2` | PA4 | D2 (green) |
| 13 `/WAKE_UP` | PA0 | unrouted; wanted for MCP2515 `~INT` (see [fixes](#outstanding-rev-01-fixes)) |
| 14 `BOOT` | PH3 | SW3 to +3V3 (bootloader select, not a Zephyr GPIO) |
| 16 `GPIO_2` | PA5 | `SPI_SCK` |
| 17 `GPIO_3` | PA6 | `SPI_MISO` |
| 18 `GPIO_4` | PB2 | `SPI_CS_CAN` |
| 19 `UTXD1` | PA9 | J5 pin 3 (console TX) |
| 20 `LED_1` | PA7 | D3 (green) |
| 21 `GPIO_5` | PA8 | unrouted; was `BAT_ON` (see [fixes](#outstanding-rev-01-fixes)) |
| 22 `GPIO_6` | PA12 | `SPI_MOSI` |
| 23 `GPIO_7` | PB6 | `PWRFLT` |
| 24 `URXD1` | PA10 | J5 pin 1 (console RX) |
| B1 / B5 | PA14 / PA13 | J5 SWDCLK / SWDIO |

The module's `RESERVED` pads are reserved only against the factory AT
firmware, which uses them internally. On the silicon they are ordinary
STM32 GPIOs - Oceanus-I's own Zephyr board drives four of them as SPI1 -
so under AURORA they are spare I/O. AUX-Tel rev 01 leaves all six
unrouted:

| Daphnis pin | STM32 | Useful alternate function |
|---|---|---|
| 25 | PA11 | `SPI1_MISO`, `USART1_CTS` |
| 28 | PA15 | `SPI1_NSS` |
| 29 | PB4 | `SPI1_MISO`, `USART1_CTS` |
| 30 | PB5 | `SPI1_MOSI` |
| 31 | PB3 | `SPI1_SCK` |
| B2 | PA1 | `SPI1_SCK`, `LPUART1_RTS` |

PA8 (pin 21, `GPIO_5`) joined them in effect: it carries no net on rev 01
since `BAT_ON` was dropped off it, and it is `SPI2_SCK` on the silicon.

Routing one of these in a later revision is the clean way to give firmware
control over the MCP2515 `~RESET` line.

### AUX-Stack data connectors

AUX-Tel exposes two `auxspaceev,aux-stack-data-connector` connectors,
`top_data_connector` and `bottom_data_connector`. Unlike AUX-Core, the two
are wired in parallel: the information signals land on the same module
pins, `CAN_H`/`CAN_L` run straight through between J1 and J2, and the UART
and I2C pairs are bridged top-to-bottom through 0 Ω links (R23/R24 for
UART, R25/R26 for I2C). The remaining 0 Ω links are taps, not bridges:
R21/R22 hang the TCAN334 off the CAN pair and R19/R20 hang LPUART1 off the
UART pair. Both connectors therefore share one CAN bus (`aux_data_can`)
and one UART (`aux_data_uart` / `lpuart1`).

I2C is a pure pass-through on this board - nothing on AUX-Tel sits on it -
so neither connector declares an `i2c-bus`, and shields that need I2C must
attach to a brain board further down the stack.

Each connector breaks out the stack-wide "information" signals as GPIOs,
defined in
`<auxspace/dt-bindings/gpio/auxspaceev-aux-stack-data-connector.h>`.
The asserted levels below come from the Baseboard sheet annotations:

| Signal | Direction on AUX-Tel | Asserted | Purpose |
|---|---|---|---|
| `AUX_DATA_ARM` | in | high | Stack-wide arm/disarm state |
| `AUX_DATA_LIFTOFF` | in | high | Stack-wide liftoff indication |
| `AUX_DATA_PWRFLT` | in | low | Stack-wide power-fault signal |
| `AUX_DATA_BAT_ON` | *not wired* | high | Request the external battery rail |

All three wired signals are inputs on AUX-Tel - AUX-Core and AUX-Pwr drive
them - and are read through the connector nexus directly, with the
consumer supplying the polarity flag the way AUX-Core's `arm_interlock`
does.

`AUX_DATA_BAT_ON` is **absent from both `gpio-map`s**, because rev 01
leaves PA8 unconnected (see
[Outstanding rev-01 fixes](#outstanding-rev-01-fixes)). Referencing it
from an overlay is therefore a build error rather than a silent no-op,
which is deliberate. With it gone AUX-Tel drives no stack signal at all,
so this board has no `stack_outputs` node.

### Inter-board link

Telemetry from AUX-Core reaches AUX-Tel over the stack UART, not over CAN.
The connector pins are named from the brain board's point of view, so
Daphnis `LPUTXD1` drives the connector's `UART_RX` pin and `LPURXD1`
listens on `UART_TX`; this mates with AUX-Core's `aux_data_uart_*` without
a crossover.

The MCP2515 is wired and described in devicetree but left
`status = "disabled"`, since CAN is not the transport for this link. An
application overlay that wants the peer CAN bus enables it with:

```dts
&aux_data_can {
	status = "okay";
};
```

`&spi1` itself is already `status = "okay"` in the board definition, so
this one node is all an overlay has to touch - a disabled parent bus would
instead fail the build with an undeclared `__device_dts_ord_*` for
`spi@40013000`, which reads like a driver bug rather than a missing
`status`. Leaving the bus enabled costs nothing until an application turns
on `CONFIG_SPI`; in a build that already uses SPI (any LoRa application,
since `subghzspi` is an SPI device) instantiating SPI1 adds roughly 600 B
of flash and claims PA5/PA6/PA12 plus the PB2 chip select.

```{warning}
Enabling the node gets you a controller that builds, initialises and then
never reports anything, because `~INT` is unrouted on rev 01 - see
[Outstanding rev-01 fixes](#outstanding-rev-01-fixes).
```

See {doc}`the telemetry library </lib/telemetry>` for the backend
structure a UART link plugs into.

(outstanding-rev-01-fixes)=
### Outstanding rev-01 fixes

This board definition is synced against AUX-Tel `e71a667` (2026-11-03).

#### Already cut

- **`SPI_SCK` moved from Daphnis pin 2 (PB8) to pin 16 (PA5), and `ARM`
  took pin 2 in exchange** (AUX-Tel `d2d27a3`, "fixed SPI-Bus"). PB8 has
  no SPI alternate function on the STM32WLE5, while PA5 is `SPI1_SCK`. The
  `Lora` sheet now carries an STM32/Daphnis/function table recording the
  choice. Devicetree already described the post-swap wiring, so nothing
  moved with it.

#### Still open

1. **MCP2515 `~INT` (U4 pin 12) is still a no-connect**, and Daphnis pin
   13 (PA0, `/WAKE_UP`) went the other way: from a labelled net to an
   unconnected pad. Zephyr's `microchip,mcp2515` binding has `int-gpios`
   as a required property, so `aux_data_can` has to name some pin; it
   names PA0 because that is the module's wake-capable input and the
   obvious place to land a CAN interrupt. **The MCP2515 therefore still
   cannot be used**, and the node stays `status = "disabled"`. Enabling
   it over an overlay gets you a controller that never reports anything.
2. **`BAT_ON` lost its only driver.** Daphnis pin 21 (PA8, `GPIO_5`) used
   to carry the stack's `Bat on ext` net; it is now an unconnected pad,
   and `Bat on ext` merely bridges J1 pin 10 to J2 pin 10. AUX-Tel can no
   longer request the battery rail, so `AUX_DATA_BAT_ON` has been dropped
   from both connector `gpio-map`s. The schematic's own annotation still
   reads "Input: Bat enable when high", so routing PA8 back to that net is
   all it would take to restore the function.

Two further issues are known but not worked around in devicetree, because
no software setting can compensate for them:

- MCP2515 `~RESET` (U4 pin 17) is a no-connect, i.e. floating. A pull-up
  to +3V3 is enough; routing one of the spare module pads to it would give
  firmware control instead.
- TCAN334 `STB` and `SHDN` (U3 pins 8 and 5) are no-connects, with no
  external pull on either. Tie them low for normal mode rather than
  relying on internal biasing. This is also why the transceiver is a plain
  `can-transceiver` child node here and not the `can-transceiver-gpio`
  AUX-Core uses for its own TCAN334.

```{note}
An earlier version of this page claimed D1/D2/D3 were drawn backwards.
They are not: the netlist has all three cathode-to-GND with the anode on a
series resistor up to the driver, so `LED_1`/`LED_2` source current and
are active HIGH, exactly as user manual chapter 5.9 describes. The
`status_leds` nodes are `GPIO_ACTIVE_HIGH` and correct. The series
resistors did change in rev 01, 392R -> 1k, so the LEDs are dimmer than on
the first boards.
```

# Programming and Debugging

```{zephyr:board-supported-runners}
```

SWD on the J5 header is the only programming path. The Daphnis-I has no
USB, and its UART bootloader will not accept a Zephyr image (see
[Why the UART bootloader is unusable](#why-the-uart-bootloader-is-unusable)).
AUX-Tel boots through MCUboot in **single-app mode**, so build with
`--sysbuild`. With SWD as the only programming path a second image slot would
never be filled, so there is just one:

| Partition | Offset    | Size   |
|-----------|-----------|--------|
| `mcuboot` | `0x00000` | 48 kB  |
| `image-0` | `0x0c000` | 200 kB |
| `storage` | `0x3e000` | 8 kB   |

MCUboot itself takes about 25 kB. The rest of its partition is headroom
for serial recovery. It checks the image's ECDSA P-256 signature before
every boot. Unless `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` names a key of your
own, sysbuild signs with MCUboot's **public** development key, which is fine on
the bench, not for EUROC.

```{warning}
Flashing AURORA replaces Würth's firmware, and you cannot put it back.

A stock Daphnis-I carries two components (user manual chapter 14): a
**secure bootloader** that performs signature-checked updates over the
Bootloader UART, and the **application firmware** (the LoRaWAN /
WE-ProWare AT stack). Only the application can be updated over UART, and
only from WE's encrypted `.sfb` package - the bootloader verifies the
image signature, so an unsigned Zephyr image is rejected there.

SWD is the only way in, and it writes raw flash from zero: WE's
bootloader goes with the application. Restoring a stock module would need
both components back, but WE distributes the application only as an `.sfb`
for their own bootloader ("available on request by contacting support")
and does not publish the bootloader at all - chapter 13 states that
firmware development details are "not available for the public". The
`.sfb` is useless once the bootloader it feeds is gone.

This is not really a choice for this board: an MCP2515 on the module's SPI
pins cannot be driven by the AT firmware, so custom firmware is the entire
point. If you would rather not overwrite anything on future builds, order
Oceanus-I (2618011182000) - the same hardware shipped blank, and the
variant Zephyr supports upstream.
```

## Building

```bash
west build -p --sysbuild -b tel65 <application>
```

`west flash` then writes MCUboot and the signed `zephyr.signed.hex` in
order. A build without `--sysbuild` links the application at the bottom
of flash and overwrites MCUboot when flashed.

`hal_stm32` and `loramac-node` must be present in the workspace; both were
added to the manifest allowlist for this board, so run `west update` once
after pulling the change.

The kernel runs on SysTick by default. LPTIM1 off the LSE is already
selected as `zephyr,system-timer`, but `CONFIG_STM32_LPTIM_TIMER`
additionally depends on `CONFIG_PM`, so setting `CONFIG_PM=y` is what
actually moves the tick onto the low-power timer.

## Probe and wiring

J5 is a 2x4 2.54 mm header (Würth 61300821021) carrying SWD, the
bootloader UART and power. It grew from 2x3 in rev 01: pins 5 and 6 used
to be GND and now carry +3V3, with GND moved to the new pins 7 and 8.
Check which header you have before clipping a probe on.

| J5 pin | Signal | STM32 |
|---|---|---|
| 1 | `URXD1` | PA10 |
| 2 | `SWDCLK` | PA14 |
| 3 | `UTXD1` | PA9 |
| 4 | `SWDIO` | PA13 |
| 5, 6 | +3V3 | - |
| 7, 8 | GND | - |

`openocd` is the default runner, because it ships an `stm32wlx` target and
needs no extra packs. `support/openocd.cfg` selects `interface/cmsis-dap.cfg`,
which covers an ST-Link on DAP firmware as well as a Raspberry Pi Pico
flashed with `debugprobe_on_pico.uf2`:

| Pico (debugprobe) | J5 |
|---|---|
| GP2 | pin 2 (`SWDCLK`) |
| GP3 | pin 4 (`SWDIO`) |
| GND | pin 7 or 8 |

```{warning}
J5 pins 5 and 6 sit directly on the TPS7B8601's output rail, so they both
feed the board and are fed by it. Pick exactly one source: either power
AUX-Tel from its VBus rail through the jumper-selected LDO **or** feed 3V3
in on pins 5/6 with no VBus jumper fitted. Driving the LDO output from a
probe while the LDO is also regulating back-feeds it.

On the older 2x3 header these two pins were GND, so a probe harness built
for a rev-01 board from before `e71a667` will short 3V3 to the probe
ground if reused unchanged.
```

For a classic ST-Link V2, change the first line of
`support/openocd.cfg` to `source [find interface/stlink.cfg]`.

The other runners work too: `jlink` (Würth's own recommendation in ANR036),
`stm32cubeprogrammer`, and `pyocd` - though pyocd has no built-in target for
this part and needs `pyocd pack install stm32wle5ccux` first. Select one
with `west flash -r <runner>`.

```{note}
J5 breaks out SWD only: `/RESET` is on SW4, not on the header, so
`support/openocd.cfg` declares no SRST line and the core is reset over SWD
instead. If OpenOCD cannot halt the core on connect, hold SW4 while it
starts, or tap SW4 as it connects.

(The reset button was SW2 before AUX-Tel `313e66d`; rev 01 now has SW3 =
BOOT and SW4 = RESET, both the same 6-pin Würth WS-TASU part.)
```

## First-time bring-up of a new module

A factory Daphnis-I is locked, so the first flash of any given module takes
three steps in this order. Steps 1 and 2 are needed exactly once per
module.

### 1. Read the DevEUI - last chance

The factory DevEUI comes from WE's `00:80:E1` OUI block and is readable
only through the AT firmware, which step 2 destroys. RDP 1 blocks debugger
reads, so this interface is the only source.

Boot the module normally (do **not** hold SW3) and talk to **LPUART1 -
PA2/PA3, the stack UART, 115200 8N1** - not J5:

```text
AT+DEUI=?
```

Record it per module. Skip this only if these boards will never join a
LoRaWAN network.

### 2. Unlock read-out protection

Modules ship at **RDP level 1**: the probe connects and the core halts, but
flash is closed to the debugger, so a `west flash` fails with

```text
Info : RDP level 1 (0x00)
Error: stm32x device protected
Error: failed erasing sectors 0 to 20
```

The `clearing lockup after double fault` and `pc: 0xfffffffe` lines just
above that are the normal symptom of attaching under RDP 1, not a board
fault.

Regressing to RDP level 0 is permitted and performs a **mandatory mass
erase** - this is the irreversible step. Level 1 is recoverable; only level
2 would be permanent, and these modules are not shipped that way.
`stm32wlx.cfg` drives flash through OpenOCD's `stm32l4x` driver:

```bash
OCD=~/zephyr-sdk-1.0.1/hosttools/usr/bin/openocd

$OCD \
  -s aurora/boards/auxspace/AUX-Tel/stm32wl/support \
  -s ~/zephyr-sdk-1.0.1/hosttools/opt/openocd/share/openocd/scripts \
  -f openocd.cfg \
  -c init \
  -c "stm32l4x unlock 0" \
  -c "stm32l4x option_load 0" \
  -c shutdown
```

`unlock` writes RDP level 0 into the option bytes; `option_load` triggers
OBL_LAUNCH, which reloads them and runs the erase. The target resets during
`option_load`, so OpenOCD losing the connection there is expected, not a
failure. Power-cycling the board instead of `option_load` works too.
Useful given J5 carries no reset line; tapping SW4 does the same.

### 3. Flash

The chip is now blank at RDP 0, and the normal flow works:

```bash
west flash
```

Check that the build directory `west flash` picks up is actually a `tel65`
build - a stale image for another board writes happily and then sits there
dead.

(why-the-uart-bootloader-is-unusable)=
## Why the UART bootloader is unusable

Kept as a record, since the conclusion rests on measurement rather than
documentation and may differ on a future firmware batch.

J5 pins 3 and 1 are PA9/PA10, which is both the Daphnis bootloader UART
*and* where the STM32WLE5's ROM system bootloader listens (AN2606: USART1,
8 bits / even parity / 1 stop, `0x7F` auto-baud). Which of the two answers
depends on the `nSWBOOT0` option byte, and WE documents neither its value
nor the behaviour.

**Measured on rev 01: the ROM bootloader is not reachable.** Holding SW3
through a reset blinks D3 on a ~3 s cycle - exactly the `LED_1` bootloader
indicator from user manual chapter 5.9 (table 10: "Turns ON for 3000 ms,
Turns OFF for 3000 ms", `LED_2` off) - and `stm32flash` reports
`Failed to init device`, i.e. no ACK to the `0x7F` auto-baud byte. So WE
cleared `nSWBOOT0`, their secure bootloader owns the BOOT pin, and these
two pins accept signed `.sfb` over YMODEM only.

To re-check this on another module: enter bootloader mode by holding
**SW3** (BOOT), tapping **SW4** (RESET), then releasing SW3, the "press at
startup to join Download Modus" note on the schematic, and probe with the
`stm32flash` runner (the tool is separate from the Zephyr SDK):

```bash
west flash -r stm32flash --device=<port> --action=info
```

A valid chip ID back would mean the ROM bootloader is exposed, and
`west flash -r stm32flash --device=<port> --verify` would then work without
any SWD probe. Note that it would not avoid the mass erase - it changes
only the wire, not the outcome.

## Radio bring-up

ANR036 points at the plain LoRa driver samples for this module family,
which are the quickest way to prove the RF front-end pins are right:

```bash
west build -p --sysbuild -b tel65 $ZEPHYR_BASE/samples/drivers/lora/send
west build -p --sysbuild -b tel65 $ZEPHYR_BASE/samples/drivers/lora/receive
```

Running `send` on this board against `receive` on a
`we_oceanus1ev` (or a second AUX-Tel) is the cheapest check that
`antenna-enable-gpios` and `tx-enable-gpios` are correct - a wrong switch
pin shows up as a clean boot with nothing on the air.
