# SPDX-License-Identifier: Apache-2.0

# SWD on J5 pins 2 and 4 is the only working path - see the board doc's
# "Why the UART bootloader is unusable". Note that a factory module sits at
# RDP level 1, so the first flash of any new module needs the one-time
# unlock documented under "First-time bring-up of a new module"; until then
# every runner here fails with "stm32x device protected".
board_runner_args(pyocd "--target=stm32wle5ccux")
board_runner_args(jlink "--device=STM32WLE5CC" "--speed=4000" "--reset-after-load")
board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")

# UTXD1/URXD1 on J5 pins 3 and 1 are PA9/PA10, which is also where the
# STM32WLE5's ROM system bootloader listens (AN2606: USART1, 8E1, 0x7F
# auto-baud) - but only if nSWBOOT0 is at its factory default. Measured on
# rev 01 it is not: WE's own secure bootloader owns the BOOT pin and takes
# signed .sfb over YMODEM only, so this runner reports "Failed to init
# device". Kept so the result can be re-checked on a future firmware batch:
#
#   west flash -r stm32flash --device=<port> --action=info
#
board_runner_args(stm32flash "--baud-rate=115200")

# OpenOCD needs no CMSIS pack and ships an stm32wlx target, so it is the
# readiest option for an ST-Link or a Pico running debugprobe. pyocd wants
# `pyocd pack install stm32wle5ccux` first.
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stm32flash.board.cmake)
