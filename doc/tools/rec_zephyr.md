# rec_zephyr.py

MicroPython-based ground station for receiving AURORA telemetry over an
HC-12 radio link during bench tests.

## Background

The flight computer broadcasts state-machine updates over an HC-12 radio
using the wire format defined in `aurora/lib/telemetry/hc12/hc12.c`.
`rec_zephyr.py` runs on a Raspberry Pi Pico flashed with MicroPython,
listens to a paired HC-12 over UART, validates each frame and prints a
human-readable line per packet to the REPL.

It is intentionally self-contained: no host PC, no extra dependencies,
no logging stack. The goal is to confirm that frames are being received
and decoded correctly before integrating a more sophisticated tool.

## Hardware

````{tip}
The hardware is identical for receiver and transmitter, since the HC-12
can do both.
````

![hardware_setup](/img/sensor_board_bench.drawio.svg)

| Pico pin | HC-12 pin | Notes |
|---|---|---|
| GP3 | SET | OPTIONAL (not implemented on receiver) |
| GP4 (UART1 TX) | RXD | |
| GP5 (UART1 RX) | TXD | |
| 3V3 | VCC | HC-12 is 3.3 V tolerant |
| GND | GND | |

The HC-12 must be set to the same UART baud as `BAUD` in the script
(factory default 9600). To change the air baud, use the firmware shell
on the transmitting side:

```
telemetry hc12 baud 19200
```

This persists the new baud to the HC-12's internal flash. Both the
transmitter HC-12 and the receiver HC-12 must be reconfigured for the
link to work.

## Wire format

Frames sent by the firmware look like this:

```
[A5 5A] [type] [payload_len] [payload...] [crc16_le]
```

| Field | Size | Purpose |
|---|---|---|
| Magic | 2 B | Lets the receiver re-sync after dropped bytes (`0xA5 0x5A`). |
| Type | 1 B | Frame kind. Currently only `0x01` (SM update) is defined. |
| Length | 1 B | Length of the payload in bytes. |
| Payload | N B | Type-specific body (see below). |
| CRC | 2 B | CRC-16/CCITT, reflected, init `0xFFFF`, over `[type ‖ len ‖ payload]`. Little-endian. |

The SM-update payload (`type = 0x01`, 64 B) matches
`struct hc12_sm_update_payload`: a millisecond timestamp, the flight
state enum, an armed flag, a reserved field, then altitude / total
acceleration / vertical acceleration / vertical velocity, then three
orientation angles (yaw, pitch, roll). All floats are 64-bit
little-endian.

## Why the parser is structured this way

A naïve receive loop calls `print()` for each frame and reads bytes
synchronously. This appears to work at high telemetry rates but fails
at lower rates because the UART hardware FIFO on the RP2040 is only
32 bytes and overruns whenever `print()` blocks. The script avoids that
problem in three ways:

1. **Driver-side buffering**: the UART is opened with `rxbuf=2048`, so
   bytes keep arriving while Python is busy.
2. **Allocation-free parsing**: one preallocated `bytearray` holds the
   in-flight bytes; parsing walks it with index arithmetic and re-uses
   the buffer across iterations.
3. **Deferred output**: decoded lines are queued and only flushed when
   the UART has no pending bytes. The slow REPL `print()` never blocks
   the parser.

The CRC is computed via a precomputed 256-entry lookup table, which is
roughly an order of magnitude faster than the bit-by-bit reference
implementation.

## Usage

````{tip}
If you only want to build a simple HC-12 telemetry tester on a bench-target,
you can do so.
An example for the pico2w bench build is described in
[Building the Pico 2 W bench target](#building-the-pico-2-w-bench-target).
````

1. Flash MicroPython onto the Pico
   ([micropython.org/download](https://micropython.org/download)).
2. Copy `rec_zephyr.py` to the Pico as `main.py` (so it runs at boot),
   for example with `mpremote`:

   ```bash
   mpremote cp tools/rec_zephyr.py :main.py
   ```

3. Open the REPL (`mpremote` or any serial terminal at 115200 baud on
   the Pico's USB CDC). One line should print per received telemetry
   frame:

   ```
   [OK ] SM t=12345 ms  state=ARMED     armed=1  alt=+0.12  a=+9.81  ...
   ```

   Frames whose CRC fails appear as `[BAD] type=0x?? len=N`: occasional
   bad frames are expected at long range or with weak antennas, but a
   steady stream of them indicates the UART baud is wrong on one side.

Here is an example output of the first successfull MULTIMETER flight:

```bash
[OK ] SM t=18740 ms  state=ARMED     armed=1  alt=-0.05  a=+10.03  av=-0.01  v=-0.11  ypr=-3.56/+2.90/-1.22
[OK ] SM t=18843 ms  state=ARMED     armed=1  alt=-0.01  a=+10.01  av=-0.05  v=-0.06  ypr=-4.45/+3.23/-1.23
[OK ] SM t=18945 ms  state=ARMED     armed=1  alt=-0.03  a=+10.62  av=+0.58  v=-0.08  ypr=-2.90/+2.43/-1.24
[OK ] SM t=19047 ms  state=ARMED     armed=1  alt=-0.02  a=+15.64  av=+5.61  v=+0.07  ypr=-2.25/+1.68/-1.80
[OK ] SM t=19150 ms  state=ARMED     armed=1  alt=+0.03  a=+37.58  av=+27.49  v=+1.66  ypr=-2.51/+2.82/-5.80
[OK ] SM t=19252 ms  state=ARMED     armed=1  alt=+0.31  a=+50.87  av=+40.86  v=+5.52  ypr=-0.16/+0.08/-8.97
[OK ] SM t=19355 ms  state=ARMED     armed=1  alt=+1.12  a=+35.49  av=+25.44  v=+8.99  ypr=+1.02/+2.13/-11.88
[OK ] SM t=19456 ms  state=ARMED     armed=1  alt=+2.24  a=+28.87  av=+18.66  v=+11.21  ypr=+2.39/+0.72/-14.61
[OK ] SM t=19559 ms  state=ARMED     armed=1  alt=+3.54  a=+25.89  av=+15.31  v=+13.01  ypr=+1.40/-1.42/-12.33
[OK ] SM t=19662 ms  state=ARMED     armed=1  alt=+4.88  a=+22.46  av=+11.72  v=+14.29  ypr=-0.73/-1.17/-6.28
[OK ] SM t=19764 ms  state=ARMED     armed=1  alt=+6.48  a=+21.55  av=+10.86  v=+15.50  ypr=-1.86/+0.66/+1.61
[OK ] SM t=19867 ms  state=ARMED     armed=1  alt=+8.19  a=+21.81  av=+11.34  v=+16.71  ypr=-1.11/+4.88/+10.68
[OK ] SM t=19968 ms  state=BOOST     armed=1  alt=+10.00  a=+21.84  av=+11.71  v=+17.94  ypr=+3.00/+3.72/+19.59
[OK ] SM t=20070 ms  state=BOOST     armed=1  alt=+11.87  a=+21.33  av=+10.50  v=+19.13  ypr=+2.45/-4.48/+31.19
[OK ] SM t=20174 ms  state=BOOST     armed=1  alt=+13.93  a=+20.76  av=+9.03  v=+20.19  ypr=-5.45/+0.74/+44.32
[OK ] SM t=20275 ms  state=BOOST     armed=1  alt=+16.00  a=+20.41  av=+9.46  v=+21.08  ypr=+0.05/+5.76/+57.49
[OK ] SM t=20378 ms  state=BOOST     armed=1  alt=+18.10  a=+20.20  av=+9.99  v=+21.98  ypr=+3.91/+0.03/+71.50
[OK ] SM t=20480 ms  state=BURNOUT   armed=1  alt=+20.33  a=+1.97  av=-9.51  v=+22.06  ypr=-71.02/-50.08/+87.94
[OK ] SM t=20582 ms  state=BURNOUT   armed=1  alt=+22.47  a=+1.43  av=-11.00  v=+20.88  ypr=-138.33/+11.62/+104.11
[OK ] SM t=20685 ms  state=BURNOUT   armed=1  alt=+24.42  a=+2.04  av=-10.81  v=+19.60  ypr=+120.82/+16.06/+119.71
[OK ] SM t=20787 ms  state=BURNOUT   armed=1  alt=+26.32  a=+1.42  av=-10.17  v=+18.50  ypr=+140.04/-49.49/+136.49
[OK ] SM t=20889 ms  state=BURNOUT   armed=1  alt=+28.01  a=+1.10  av=-10.48  v=+17.29  ypr=-130.06/+0.50/+153.48
[OK ] SM t=20992 ms  state=BURNOUT   armed=1  alt=+29.66  a=+0.85  av=-10.43  v=+16.15  ypr=-135.92/+9.07/+169.21
[OK ] SM t=21094 ms  state=BURNOUT   armed=1  alt=+31.16  a=+0.71  av=-10.69  v=+14.96  ypr=-179.08/+32.83/-176.29
[OK ] SM t=21197 ms  state=BURNOUT   armed=1  alt=+32.52  a=+0.63  av=-10.65  v=+13.72  ypr=+140.31/-21.45/-161.97
[OK ] SM t=21299 ms  state=BURNOUT   armed=1  alt=+33.77  a=+0.61  av=-10.30  v=+12.56  ypr=-128.05/-23.99/-148.44
[OK ] SM t=21401 ms  state=BURNOUT   armed=1  alt=+34.88  a=+0.90  av=-10.16  v=+11.39  ypr=-109.98/+3.06/-135.81
[OK ] SM t=21504 ms  state=BURNOUT   armed=1  alt=+35.88  a=+0.92  av=-10.16  v=+10.21  ypr=-108.25/-2.99/-124.20
[OK ] SM t=21606 ms  state=BURNOUT   armed=1  alt=+36.90  a=+0.34  av=-10.72  v=+9.19  ypr=+164.36/-42.75/-113.24
[OK ] SM t=21709 ms  state=BURNOUT   armed=1  alt=+37.78  a=+0.39  av=-10.89  v=+8.06  ypr=+119.36/-24.85/-102.71
[OK ] SM t=21811 ms  state=BURNOUT   armed=1  alt=+38.60  a=+0.20  av=-10.58  v=+7.02  ypr=-125.22/-10.88/-92.64
[OK ] SM t=21913 ms  state=BURNOUT   armed=1  alt=+39.28  a=+0.80  av=-10.12  v=+5.99  ypr=-100.30/+36.70/-82.96
[OK ] SM t=22017 ms  state=BURNOUT   armed=1  alt=+39.88  a=+0.64  av=-10.24  v=+4.96  ypr=-95.44/+19.17/-72.87
[OK ] SM t=22118 ms  state=BURNOUT   armed=1  alt=+40.35  a=+0.47  av=-10.35  v=+3.95  ypr=-101.31/+33.50/-62.83
[OK ] SM t=22220 ms  state=BURNOUT   armed=1  alt=+40.74  a=+0.58  av=-10.18  v=+2.95  ypr=-94.90/+54.91/-54.07
[OK ] SM t=22322 ms  state=BURNOUT   armed=1  alt=+41.04  a=+0.61  av=-10.14  v=+1.96  ypr=-104.04/+67.25/-46.65
[OK ] SM t=22425 ms  state=BURNOUT   armed=1  alt=+41.27  a=+0.45  av=-10.27  v=+1.01  ypr=+105.94/+81.01/-39.79
[OK ] SM t=22528 ms  state=BURNOUT   armed=1  alt=+41.44  a=+0.33  av=-10.30  v=+0.08  ypr=-100.30/+71.28/-33.34
[OK ] SM t=22629 ms  state=BURNOUT   armed=1  alt=+41.46  a=+0.38  av=-10.19  v=-0.89  ypr=-90.00/+77.01/-27.10
[OK ] SM t=22732 ms  state=MAIN      armed=1  alt=+41.25  a=+0.72  av=-9.97  v=-2.01  ypr=-121.37/+50.38/-22.15
[OK ] SM t=22834 ms  state=MAIN      armed=1  alt=+40.91  a=+0.39  av=-10.11  v=-3.14  ypr=-45.00/+88.02/-19.37
[OK ] SM t=22937 ms  state=MAIN      armed=1  alt=+40.41  a=+1.24  av=-9.62  v=-4.26  ypr=-43.60/+63.43/-15.98
[OK ] SM t=23040 ms  state=MAIN      armed=1  alt=+39.72  a=+1.15  av=-9.36  v=-5.45  ypr=-122.47/+83.75/-12.21
[OK ] SM t=23141 ms  state=MAIN      armed=1  alt=+38.89  a=+1.45  av=-8.85  v=-6.62  ypr=+120.47/+67.09/-7.60
[OK ] SM t=23244 ms  state=MAIN      armed=1  alt=+38.09  a=+1.06  av=-9.09  v=-7.62  ypr=+135.43/+31.66/-1.29
[OK ] SM t=23347 ms  state=MAIN      armed=1  alt=+37.15  a=+1.37  av=-8.66  v=-8.66  ypr=+129.69/+24.78/+7.51
[OK ] SM t=23448 ms  state=MAIN      armed=1  alt=+36.12  a=+3.37  av=-6.77  v=-9.52  ypr=+129.98/+42.77/+11.86
[OK ] SM t=23552 ms  state=MAIN      armed=1  alt=+35.05  a=+6.12  av=-4.18  v=-10.16  ypr=+156.14/+50.40/+13.17
[OK ] SM t=23653 ms  state=MAIN      armed=1  alt=+34.04  a=+5.00  av=-5.24  v=-10.43  ypr=+146.69/+49.61/+23.01
[OK ] SM t=23755 ms  state=MAIN      armed=1  alt=+33.30  a=+11.51  av=+0.84  v=-10.37  ypr=+115.04/+38.72/+55.42
[OK ] SM t=23859 ms  state=MAIN      armed=1  alt=+33.78  a=+23.17  av=+9.19  v=-8.52  ypr=+100.31/+5.78/+93.51
[OK ] SM t=23960 ms  state=MAIN      armed=1  alt=+34.23  a=+29.01  av=+7.87  v=-7.08  ypr=+53.13/-29.36/+117.47
[OK ] SM t=24062 ms  state=MAIN      armed=1  alt=+34.02  a=+34.33  av=+20.09  v=-4.37  ypr=-79.12/-2.73/+98.43
[OK ] SM t=24165 ms  state=MAIN      armed=1  alt=+33.73  a=+16.94  av=+2.15  v=-3.90  ypr=-29.83/+37.86/+2.75
[OK ] SM t=24267 ms  state=MAIN      armed=1  alt=+33.57  a=+18.95  av=+7.14  v=-2.76  ypr=-72.39/-27.93/-107.93
[OK ] SM t=24370 ms  state=MAIN      armed=1  alt=+33.32  a=+9.14  av=-2.01  v=-2.70  ypr=+69.35/-38.67/+143.15
[OK ] SM t=24472 ms  state=MAIN      armed=1  alt=+32.83  a=+18.48  av=+4.60  v=-3.06  ypr=-58.21/-25.78/+88.17
[OK ] SM t=24575 ms  state=MAIN      armed=1  alt=+32.45  a=+14.15  av=+3.93  v=-2.58  ypr=-2.67/-62.32/+32.50
[OK ] SM t=24677 ms  state=REDUNDANT armed=1  alt=+32.07  a=+12.30  av=-0.23  v=-2.61  ypr=-44.31/+1.78/+7.34
[OK ] SM t=24779 ms  state=REDUNDANT armed=1  alt=+31.58  a=+9.06  av=-2.23  v=-2.92  ypr=-39.51/-10.41/-52.86
[OK ] SM t=24882 ms  state=REDUNDANT armed=1  alt=+31.05  a=+8.83  av=-1.12  v=-3.37  ypr=+11.07/-0.93/-129.73
[OK ] SM t=24984 ms  state=REDUNDANT armed=1  alt=+30.63  a=+11.13  av=+0.58  v=-3.48  ypr=-26.86/+27.24/+167.68
[OK ] SM t=25086 ms  state=REDUNDANT armed=1  alt=+30.25  a=+9.56  av=-0.72  v=-3.52  ypr=-26.51/-21.80/+82.07
[OK ] SM t=25190 ms  state=REDUNDANT armed=1  alt=+29.84  a=+9.03  av=-0.76  v=-3.64  ypr=+26.03/-30.38/+4.41
[OK ] SM t=25291 ms  state=REDUNDANT armed=1  alt=+29.42  a=+10.74  av=+0.47  v=-3.69  ypr=-5.42/-38.53/-38.79
[OK ] SM t=25394 ms  state=REDUNDANT armed=1  alt=+28.95  a=+11.68  av=+0.69  v=-3.68  ypr=-25.77/-37.32/-51.50
[OK ] SM t=25495 ms  state=REDUNDANT armed=1  alt=+28.50  a=+10.57  av=+0.43  v=-3.76  ypr=-8.89/+4.99/-56.86
[OK ] SM t=25598 ms  state=REDUNDANT armed=1  alt=+28.01  a=+12.87  av=+2.14  v=-3.73  ypr=+21.70/+29.93/-74.80
[OK ] SM t=25702 ms  state=REDUNDANT armed=1  alt=+27.46  a=+11.52  av=+0.43  v=-3.76  ypr=+26.72/+7.02/-97.24
[OK ] SM t=25803 ms  state=REDUNDANT armed=1  alt=+26.96  a=+10.71  av=+0.02  v=-3.91  ypr=-6.69/-31.53/-90.74
[OK ] SM t=25905 ms  state=REDUNDANT armed=1  alt=+26.49  a=+10.35  av=+0.10  v=-3.98  ypr=-21.27/-2.70/-67.53
[OK ] SM t=26007 ms  state=REDUNDANT armed=1  alt=+25.97  a=+11.91  av=+0.83  v=-4.07  ypr=+27.20/+13.52/-54.07
[OK ] SM t=26110 ms  state=REDUNDANT armed=1  alt=+25.56  a=+10.38  av=-0.24  v=-4.04  ypr=+28.48/-16.06/-17.88
[OK ] SM t=26213 ms  state=REDUNDANT armed=1  alt=+25.13  a=+11.58  av=+0.76  v=-4.02  ypr=-24.11/-1.66/+42.19
[OK ] SM t=26314 ms  state=REDUNDANT armed=1  alt=+24.73  a=+9.42  av=-0.60  v=-4.04  ypr=-0.64/-2.27/+83.98
[OK ] SM t=26417 ms  state=REDUNDANT armed=1  alt=+24.35  a=+12.23  av=+1.31  v=-3.99  ypr=-35.13/-32.25/+130.56
[OK ] SM t=26520 ms  state=REDUNDANT armed=1  alt=+23.86  a=+12.32  av=+0.79  v=-3.94  ypr=-49.66/-13.34/+167.29
[OK ] SM t=26621 ms  state=REDUNDANT armed=1  alt=+23.42  a=+10.14  av=+0.16  v=-4.01  ypr=+16.62/-17.58/+166.20
[OK ] SM t=26725 ms  state=REDUNDANT armed=1  alt=+22.88  a=+10.35  av=+0.63  v=-4.09  ypr=+23.60/-27.53/+178.63
[OK ] SM t=26826 ms  state=REDUNDANT armed=1  alt=+22.47  a=+10.94  av=+0.20  v=-4.06  ypr=-27.43/-9.16/-148.33
[OK ] SM t=26929 ms  state=REDUNDANT armed=1  alt=+22.07  a=+11.03  av=-0.62  v=-4.05  ypr=-34.54/+13.91/-135.98
[OK ] SM t=27032 ms  state=REDUNDANT armed=1  alt=+21.55  a=+9.98  av=+0.12  v=-4.24  ypr=+0.58/-18.45/-154.00
[OK ] SM t=27133 ms  state=REDUNDANT armed=1  alt=+21.06  a=+11.24  av=+1.63  v=-4.18  ypr=+13.57/-32.32/-163.05
[OK ] SM t=27236 ms  state=REDUNDANT armed=1  alt=+20.62  a=+10.33  av=-0.30  v=-4.13  ypr=-23.52/-20.67/-152.69
[OK ] SM t=27338 ms  state=REDUNDANT armed=1  alt=+20.13  a=+10.06  av=-0.56  v=-4.25  ypr=-20.88/-13.03/-156.76
[OK ] SM t=27440 ms  state=REDUNDANT armed=1  alt=+19.74  a=+10.81  av=+1.08  v=-4.19  ypr=+14.52/-23.86/-178.03
[OK ] SM t=27543 ms  state=REDUNDANT armed=1  alt=+19.34  a=+10.51  av=+0.46  v=-4.04  ypr=+4.86/-25.46/-174.56
[OK ] SM t=27645 ms  state=REDUNDANT armed=1  alt=+18.82  a=+10.49  av=-0.86  v=-4.19  ypr=-26.64/-2.25/-144.89
[OK ] SM t=27747 ms  state=REDUNDANT armed=1  alt=+18.29  a=+10.38  av=-0.37  v=-4.38  ypr=-27.28/-16.72/-129.82
[OK ] SM t=27850 ms  state=REDUNDANT armed=1  alt=+17.98  a=+11.64  av=+1.90  v=-4.15  ypr=+6.42/-47.46/-127.63
[OK ] SM t=27952 ms  state=REDUNDANT armed=1  alt=+17.52  a=+10.59  av=+0.48  v=-4.08  ypr=-16.84/-32.60/-103.78
[OK ] SM t=28055 ms  state=REDUNDANT armed=1  alt=+17.07  a=+10.48  av=-0.34  v=-4.11  ypr=-8.98/-0.42/-80.02
[OK ] SM t=28157 ms  state=REDUNDANT armed=1  alt=+16.62  a=+10.15  av=-0.35  v=-4.22  ypr=-4.07/-5.63/-69.58
[OK ] SM t=28259 ms  state=REDUNDANT armed=1  alt=+16.14  a=+11.87  av=+1.43  v=-4.20  ypr=-21.74/-29.99/-63.51
[OK ] SM t=28362 ms  state=REDUNDANT armed=1  alt=+15.77  a=+10.67  av=+0.35  v=-4.02  ypr=-18.91/-34.15/-64.91
[OK ] SM t=28464 ms  state=REDUNDANT armed=1  alt=+15.27  a=+9.05  av=-0.91  v=-4.19  ypr=+2.27/-24.84/-73.26
[OK ] SM t=28567 ms  state=REDUNDANT armed=1  alt=+14.77  a=+10.39  av=+0.31  v=-4.29  ypr=+5.17/-19.77/-77.38
[OK ] SM t=28669 ms  state=REDUNDANT armed=1  alt=+14.23  a=+10.59  av=-0.36  v=-4.41  ypr=-17.64/-25.31/-69.76
[OK ] SM t=28771 ms  state=REDUNDANT armed=1  alt=+13.73  a=+11.29  av=-0.03  v=-4.50  ypr=-27.48/-28.16/-62.40
[BAD] type=0x01 len=64
[OK ] SM t=28976 ms  state=REDUNDANT armed=1  alt=+12.78  a=+10.02  av=-0.24  v=-4.42  ypr=-9.38/-28.42/-34.69
[OK ] SM t=29079 ms  state=REDUNDANT armed=1  alt=+12.42  a=+9.48  av=-1.36  v=-4.41  ypr=-34.75/-20.19/+2.78
[OK ] SM t=29180 ms  state=REDUNDANT armed=1  alt=+11.86  a=+10.57  av=+0.19  v=-4.67  ypr=-32.01/-26.97/+18.91
[OK ] SM t=29283 ms  state=REDUNDANT armed=1  alt=+11.27  a=+10.94  av=+0.94  v=-4.64  ypr=-8.01/-29.92/+14.32
[OK ] SM t=29386 ms  state=REDUNDANT armed=1  alt=+10.72  a=+11.82  av=+1.79  v=-4.58  ypr=+6.41/-21.76/+6.24
[OK ] SM t=29488 ms  state=REDUNDANT armed=1  alt=+10.16  a=+11.95  av=+1.21  v=-4.55  ypr=-10.63/-28.15/+3.77
[OK ] SM t=29590 ms  state=REDUNDANT armed=1  alt=+9.64  a=+11.57  av=+0.68  v=-4.47  ypr=-13.95/-33.15/+1.90
[OK ] SM t=29692 ms  state=REDUNDANT armed=1  alt=+9.14  a=+9.70  av=-0.47  v=-4.49  ypr=-2.00/-25.36/+3.25
[OK ] SM t=29795 ms  state=REDUNDANT armed=1  alt=+8.63  a=+10.66  av=+0.59  v=-4.54  ypr=-1.70/-20.40/+16.20
[OK ] SM t=29898 ms  state=REDUNDANT armed=1  alt=+8.00  a=+10.97  av=+0.58  v=-4.64  ypr=-14.93/-15.17/+37.43
[OK ] SM t=29999 ms  state=REDUNDANT armed=1  alt=+7.50  a=+10.91  av=+0.78  v=-4.57  ypr=-14.05/-19.96/+48.04
[OK ] SM t=30102 ms  state=REDUNDANT armed=1  alt=+6.89  a=+10.61  av=+0.75  v=-4.67  ypr=-4.15/-24.52/+47.85
[OK ] SM t=30205 ms  state=REDUNDANT armed=1  alt=+6.37  a=+10.69  av=+0.86  v=-4.63  ypr=-0.05/-18.74/+50.64
[OK ] SM t=30306 ms  state=REDUNDANT armed=1  alt=+5.94  a=+10.13  av=+0.05  v=-4.55  ypr=-12.57/-14.50/+54.59
[OK ] SM t=30410 ms  state=REDUNDANT armed=1  alt=+5.45  a=+10.09  av=+0.08  v=-4.56  ypr=-13.20/-23.36/+48.02
[OK ] SM t=30511 ms  state=REDUNDANT armed=1  alt=+5.02  a=+10.25  av=+0.50  v=-4.49  ypr=+3.48/-30.36/+37.39
[OK ] SM t=30614 ms  state=REDUNDANT armed=1  alt=+4.54  a=+10.00  av=+0.22  v=-4.49  ypr=+0.24/-24.06/+43.96
[OK ] SM t=30717 ms  state=REDUNDANT armed=1  alt=+4.00  a=+10.85  av=+0.48  v=-4.53  ypr=-25.02/-11.96/+64.82
[OK ] SM t=30818 ms  state=REDUNDANT armed=1  alt=+3.57  a=+9.99  av=-0.23  v=-4.50  ypr=-26.87/-17.10/+71.09
[OK ] SM t=30921 ms  state=REDUNDANT armed=1  alt=+3.17  a=+11.61  av=+1.83  v=-4.34  ypr=+2.70/-34.79/+59.71
[OK ] SM t=31023 ms  state=REDUNDANT armed=1  alt=+2.72  a=+11.02  av=+1.17  v=-4.14  ypr=-5.31/-26.80/+60.16
[OK ] SM t=31125 ms  state=REDUNDANT armed=1  alt=+2.23  a=+10.20  av=+0.05  v=-4.13  ypr=-14.63/-14.91/+64.17
[OK ] SM t=31228 ms  state=REDUNDANT armed=1  alt=+1.71  a=+10.52  av=+0.61  v=-4.24  ypr=-4.06/-26.02/+55.70
[OK ] SM t=31330 ms  state=REDUNDANT armed=1  alt=+1.32  a=+11.58  av=+1.76  v=-4.03  ypr=+6.63/-33.11/+53.12
[OK ] SM t=31433 ms  state=REDUNDANT armed=1  alt=+0.87  a=+10.48  av=+0.13  v=-4.01  ypr=-21.17/-18.03/+76.64
[OK ] SM t=31535 ms  state=REDUNDANT armed=1  alt=+0.41  a=+20.39  av=+8.67  v=-3.95  ypr=+14.89/-12.80/+95.44
[OK ] SM t=31637 ms  state=REDUNDANT armed=1  alt=-0.12  a=+29.70  av=-18.36  v=-3.60  ypr=+172.30/+5.49/+101.88
[OK ] SM t=31740 ms  state=REDUNDANT armed=1  alt=-0.34  a=+22.87  av=+10.74  v=-2.73  ypr=-178.36/+6.37/+79.93
[OK ] SM t=31842 ms  state=REDUNDANT armed=1  alt=-0.40  a=+24.61  av=-25.94  v=-3.65  ypr=-177.57/+7.04/+53.45
[OK ] SM t=31944 ms  state=REDUNDANT armed=1  alt=-0.74  a=+20.47  av=-18.11  v=-6.52  ypr=+173.96/+17.27/+95.96
[OK ] SM t=32047 ms  state=REDUNDANT armed=1  alt=-1.08  a=+4.51  av=-13.62  v=-6.36  ypr=+18.12/+11.00/+120.68
[OK ] SM t=32149 ms  state=REDUNDANT armed=1  alt=-1.44  a=+8.82  av=-17.77  v=-8.41  ypr=-94.16/-13.56/+117.25
[OK ] SM t=32252 ms  state=REDUNDANT armed=1  alt=-1.83  a=+7.39  av=-13.74  v=-9.45  ypr=-42.60/-9.62/+111.86
[OK ] SM t=32354 ms  state=REDUNDANT armed=1  alt=-2.21  a=+11.35  av=-14.45  v=-10.34  ypr=-52.12/-11.03/+114.81
[OK ] SM t=32456 ms  state=REDUNDANT armed=1  alt=-2.60  a=+6.35  av=-11.62  v=-10.88  ypr=-57.47/-7.19/+119.10
[OK ] SM t=32560 ms  state=REDUNDANT armed=1  alt=-2.87  a=+6.10  av=-12.74  v=-11.11  ypr=-152.98/+12.05/+120.90
[OK ] SM t=32661 ms  state=REDUNDANT armed=1  alt=-3.20  a=+6.92  av=-11.69  v=-11.07  ypr=-99.19/+9.07/+123.14
[OK ] SM t=32763 ms  state=REDUNDANT armed=1  alt=-3.36  a=+9.97  av=-8.18  v=-10.91  ypr=-107.12/+25.84/+123.31
[OK ] SM t=32866 ms  state=REDUNDANT armed=1  alt=-3.39  a=+10.06  av=-6.86  v=-10.47  ypr=-109.49/+24.15/+123.27
[OK ] SM t=32968 ms  state=REDUNDANT armed=1  alt=-3.46  a=+10.03  av=-5.36  v=-9.96  ypr=-109.29/+24.52/+123.26
[OK ] SM t=33071 ms  state=REDUNDANT armed=1  alt=-3.49  a=+10.01  av=-4.18  v=-9.34  ypr=-109.18/+24.39/+123.25
[OK ] SM t=33172 ms  state=REDUNDANT armed=1  alt=-3.40  a=+10.00  av=-3.27  v=-8.57  ypr=-109.31/+24.29/+123.24
[OK ] SM t=33275 ms  state=REDUNDANT armed=1  alt=-3.32  a=+10.02  av=-2.53  v=-7.81  ypr=-109.23/+24.43/+123.23
[OK ] SM t=33378 ms  state=REDUNDANT armed=1  alt=-3.17  a=+10.01  av=-2.03  v=-7.00  ypr=-109.20/+24.41/+123.22
[OK ] SM t=33480 ms  state=REDUNDANT armed=1  alt=-3.07  a=+10.00  av=-1.67  v=-6.29  ypr=-109.33/+24.37/+123.22
[OK ] SM t=33583 ms  state=REDUNDANT armed=1  alt=-2.80  a=+10.01  av=-1.39  v=-5.45  ypr=-109.20/+24.41/+123.21
[OK ] SM t=33684 ms  state=REDUNDANT armed=1  alt=-2.50  a=+10.01  av=-1.21  v=-4.65  ypr=-109.40/+24.21/+123.20
[OK ] SM t=33787 ms  state=REDUNDANT armed=1  alt=-2.28  a=+10.02  av=-1.07  v=-4.01  ypr=-109.39/+24.56/+123.19
[OK ] SM t=33890 ms  state=REDUNDANT armed=1  alt=-2.13  a=+10.03  av=-0.98  v=-3.50  ypr=-109.19/+24.24/+123.19
[OK ] SM t=33991 ms  state=REDUNDANT armed=1  alt=-2.02  a=+10.01  av=-0.93  v=-3.09  ypr=-109.18/+24.39/+123.18
[OK ] SM t=34095 ms  state=REDUNDANT armed=1  alt=-1.86  a=+10.02  av=-0.88  v=-2.66  ypr=-109.50/+24.49/+123.18
[OK ] SM t=34196 ms  state=REDUNDANT armed=1  alt=-1.66  a=+10.00  av=-0.87  v=-2.23  ypr=-109.41/+24.48/+123.17
[OK ] SM t=34298 ms  state=REDUNDANT armed=1  alt=-1.49  a=+10.00  av=-0.85  v=-1.89  ypr=-109.22/+24.48/+123.17
[OK ] SM t=34402 ms  state=REDUNDANT armed=1  alt=-1.44  a=+10.04  av=-0.80  v=-1.71  ypr=-109.36/+24.27/+123.17
[OK ] SM t=34503 ms  state=REDUNDANT armed=1  alt=-1.23  a=+10.06  av=-0.77  v=-1.37  ypr=-109.29/+24.87/+123.15
[OK ] SM t=34606 ms  state=REDUNDANT armed=1  alt=-1.20  a=+9.98  av=-0.84  v=-1.27  ypr=-109.28/+24.65/+123.14
[OK ] SM t=34708 ms  state=REDUNDANT armed=1  alt=-1.10  a=+10.02  av=-0.80  v=-1.10  ypr=-109.25/+24.60/+123.13
[OK ] SM t=34810 ms  state=LANDED    armed=1  alt=-0.99  a=+10.02  av=-0.79  v=-0.94  ypr=-109.44/+24.60/+123.12
[OK ] SM t=34913 ms  state=LANDED    armed=1  alt=-0.91  a=+10.01  av=-0.81  v=-0.82  ypr=-109.37/+24.29/+123.13
[OK ] SM t=35015 ms  state=LANDED    armed=1  alt=-0.93  a=+10.03  av=-0.79  v=-0.83  ypr=-109.48/+24.47/+123.11
[OK ] SM t=35117 ms  state=LANDED    armed=1  alt=-0.91  a=+10.02  av=-0.79  v=-0.79  ypr=-109.48/+24.42/+123.11
[OK ] SM t=35220 ms  state=LANDED    armed=1  alt=-0.84  a=+10.01  av=-0.81  v=-0.71  ypr=-109.39/+24.83/+123.09
```

````{note}
State APOGEE is so short-lived that the 10Hz LoRa rate is not enough to display it.
````

## Building the Pico 2 W bench target

For bench work it is useful to flash a Pico 2 W with a minimal AURORA
build that only exercises the telemetry stack. No real IMU, no
barometer, no SD card. This is what the `rpi_pico2/rp2350a/m33/w` board
target plus the `sim` and `nodisk` snippets provide:

```bash
west build -p -b rpi_pico2/rp2350a/m33/w -S sim -S nodisk sensor_board
```

After flashing the resulting `build/zephyr/zephyr.uf2` onto the Pico,
attach the HC-12 to UART1 as documented in the overlay. The board will
boot, start the simulated flight, and emit telemetry frames over the
radio for `rec_zephyr.py` to pick up.

## Requirements

- A Raspberry Pi Pico (or Pico W / Pico 2) flashed with MicroPython.
- An HC-12 radio module paired with the transmitter on the same channel
  and baud.
- No host-side Python dependencies, the REPL is the user interface.
