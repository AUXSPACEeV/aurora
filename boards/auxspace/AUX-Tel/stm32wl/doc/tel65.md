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
AUX-Tel is still under hardware bring-up and the board definition assumes
two rev-01 netlist changes that have not been cut yet. See
[Required rev-01 fixes](#required-rev-01-fixes) before building hardware
against it.
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
  transceiver, tapped off the stack CAN pair, with 124 Ω split termination
  on board. Disabled in devicetree by default.
- Inter-board link to AUX-Core over the stack UART (LPUART1)
- J5 2x3 2.54 mm debug header: SWDIO, SWDCLK and the USART1 console
- Two green status LEDs on the module's `LED_1` / `LED_2` pins
- RESET and BOOT buttons
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
| 2 `GPIO_1` | PB8 | `ARM` (see [fixes](#required-rev-01-fixes)) |
| 5 `LPUTXD1` | PA2 | stack `UART_RX` |
| 6 `LPURXD1` | PA3 | stack `UART_TX` |
| 7 `LED_2` | PA4 | D2 (green) |
| 13 `/WAKE_UP` | PA0 | MCP2515 `~INT` (see [fixes](#required-rev-01-fixes)) |
| 14 `BOOT` | PH3 | SW3 (bootloader select, not a Zephyr GPIO) |
| 16 `GPIO_2` | PA5 | `SPI_SCK` (see [fixes](#required-rev-01-fixes)) |
| 17 `GPIO_3` | PA6 | `SPI_MISO` |
| 18 `GPIO_4` | PB2 | `SPI_CS_CAN` |
| 19 `UTXD1` | PA9 | J5 pin 3 (console TX) |
| 20 `LED_1` | PA7 | D3 (green) |
| 21 `GPIO_5` | PA8 | `BAT_ON` |
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

Routing one of them in a later revision is the clean way to give firmware
control over the MCP2515 `~RESET` line.

### AUX-Stack data connectors

AUX-Tel exposes two `auxspaceev,aux-stack-data-connector` connectors,
`top_data_connector` and `bottom_data_connector`. Unlike AUX-Core, the two
are wired in parallel: the information signals land on the same module
pins, and the CAN, UART and I2C pairs are bridged top-to-bottom through 0 Ω
links (R21..R26). Both connectors therefore share one CAN bus
(`aux_data_can`) and one UART (`aux_data_uart` / `lpuart1`).

I2C is a pure pass-through on this board - nothing on AUX-Tel sits on it -
so neither connector declares an `i2c-bus`, and shields that need I2C must
attach to a brain board further down the stack.

Each connector breaks out the four stack-wide "information" signals as
GPIOs, defined in
`<auxspace/dt-bindings/gpio/auxspaceev-aux-stack-data-connector.h>`:

| Signal | Direction on AUX-Tel | Purpose |
|---|---|---|
| `AUX_DATA_ARM` | in | Stack-wide arm/disarm state |
| `AUX_DATA_LIFTOFF` | in | Stack-wide liftoff indication |
| `AUX_DATA_PWRFLT` | in | Stack-wide power-fault signal |
| `AUX_DATA_BAT_ON` | out | Request the external battery rail |

`AUX_DATA_BAT_ON` is the only one AUX-Tel drives; it is wrapped as
`bat_on_request` under the `stack_outputs` node for convenience. The three
inputs are read through the connector nexus directly.

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

See {doc}`the telemetry library </lib/telemetry>` for the backend
structure a UART link plugs into.

(required-rev-01-fixes)=
### Required rev-01 fixes

The MCP2515 cannot be driven at all on the PCB as drawn, so this board
definition assumes two netlist changes:

1. **`SPI_SCK` moves from Daphnis pin 2 (PB8) to pin 16 (PA5), and `ARM`
   takes pin 2 in exchange.** PB8 has no SPI alternate function on the
   STM32WLE5, while PA5 is `SPI1_SCK`. The module's other two `SPI1_SCK`
   pads, PA1 and PB3, would work under our own firmware as well - see
   [the spare pads above](#daphnis-i-pin-usage) - but rev 01 leaves
   both unrouted, so swapping two already-routed nets is the cheaper fix.
2. **MCP2515 `~INT` (U4 pin 12) routes to Daphnis pin 13 (PA0,
   `/WAKE_UP`)**, which rev 01 leaves as a no-connect. Zephyr's
   `microchip,mcp2515` binding has `int-gpios` as a required property, and
   PA0 is the module's wake-capable input - which is what a CAN interrupt
   wants to be.

Three further issues are known but not worked around in devicetree,
because no software setting can compensate for them:

- MCP2515 `~RESET` (U4 pin 17) is a no-connect, i.e. floating. A pull-up
  to +3V3 is enough; routing one of the spare module pads to it would give
  firmware control instead.
- TCAN334 `STB` and `SHDN` (U3 pins 8 and 5) are no-connects. Tie them low
  for normal mode rather than relying on internal biasing.
- D1, D2 and D3 are drawn anode-to-GND with the cathode on the driver,
  which on paper is backwards - the module's `LED_1`/`LED_2` are active
  HIGH (user manual chapter 5.9), so the GPIO should source into the
  anode. The assembled rev-01 board does blink D3 in bootloader mode, so
  the physical polarity is correct and only the schematic symbols are
  inverted. Still worth fixing before the next assembly run, since the
  discrepancy could be resolved the other way next time. The `status_leds`
  nodes are `GPIO_ACTIVE_HIGH` per the user manual; if a blink test comes
  out inverted, flip them.

# Programming and Debugging

```{zephyr:board-supported-runners}
```

SWD on the J5 header is the only programming path. The Daphnis-I has no
USB, and its UART bootloader will not accept a Zephyr image (see
[Why the UART bootloader is unusable](#why-the-uart-bootloader-is-unusable)).
AUX-Tel therefore does **not** use MCUboot or `sysbuild`, unlike AUX-Core:
256 kB of flash does not leave room for a bootloader plus two image slots,
so the application starts at the bottom of flash with an 8 kB `storage`
partition at the top.

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
west build -p -b tel65 <application>
```

`hal_stm32` and `loramac-node` must be present in the workspace; both were
added to the manifest allowlist for this board, so run `west update` once
after pulling the change.

The kernel runs on SysTick by default. LPTIM1 off the LSE is already
selected as `zephyr,system-timer`, but `CONFIG_STM32_LPTIM_TIMER`
additionally depends on `CONFIG_PM`, so setting `CONFIG_PM=y` is what
actually moves the tick onto the low-power timer.

## Probe and wiring

J5 is a 2x3 2.54 mm header carrying SWD and the bootloader UART:

| J5 pin | Signal | STM32 |
|---|---|---|
| 1 | `URXD1` | PA10 |
| 2 | `SWDCLK` | PA14 |
| 3 | `UTXD1` | PA9 |
| 4 | `SWDIO` | PA13 |
| 5, 6 | GND | - |

`openocd` is the default runner, because it ships an `stm32wlx` target and
needs no extra packs. `support/openocd.cfg` selects `interface/cmsis-dap.cfg`,
which covers an ST-Link on DAP firmware as well as a Raspberry Pi Pico
flashed with `debugprobe_on_pico.uf2`:

| Pico (debugprobe) | J5 |
|---|---|
| GP2 | pin 2 (`SWDCLK`) |
| GP3 | pin 4 (`SWDIO`) |
| GND | pin 5 or 6 |

Power AUX-Tel from its own VBus rail through the jumper-selected LDO; do
not feed 3V3 in from the probe. For a classic ST-Link V2, change the first
line of `support/openocd.cfg` to `source [find interface/stlink.cfg]`.

The other runners work too: `jlink` (Würth's own recommendation in ANR036),
`stm32cubeprogrammer`, and `pyocd` - though pyocd has no built-in target for
this part and needs `pyocd pack install stm32wle5ccux` first. Select one
with `west flash -r <runner>`.

```{note}
J5 breaks out SWD only - `/RESET` is on SW2, not on the header - so
`support/openocd.cfg` declares no SRST line and the core is reset over SWD
instead. If OpenOCD cannot halt the core on connect, hold SW2 while it
starts, or tap SW2 as it connects.
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
failure. Power-cycling the board instead of `option_load` works too -
useful given J5 carries no reset line.

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
**SW3** (BOOT), tapping **SW2** (RESET), then releasing SW3 - the "press at
startup to join Download Modus" note on the schematic - and probe with the
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
west build -p -b tel65 $ZEPHYR_BASE/samples/drivers/lora/send
west build -p -b tel65 $ZEPHYR_BASE/samples/drivers/lora/receive
```

Running `send` on this board against `receive` on a
`we_oceanus1ev` (or a second AUX-Tel) is the cheapest check that
`antenna-enable-gpios` and `tx-enable-gpios` are correct - a wrong switch
pin shows up as a clean boot with nothing on the air.
