# SPDX-License-Identifier: Apache-2.0


if("${BOARD_QUALIFIERS}" MATCHES "rp2040")
  if(NOT SYSBUILD)
    message(WARNING
      "rp2040 requires sysbuild (MCUboot). "
      "Use: west build --sysbuild")
  endif()
endif()

if("${RPI_PICO_DEBUG_ADAPTER}" STREQUAL "")
  set(RPI_PICO_DEBUG_ADAPTER "cmsis-dap")
endif()

board_runner_args(openocd --cmd-pre-init "source [find interface/${RPI_PICO_DEBUG_ADAPTER}.cfg]")
board_runner_args(openocd --cmd-pre-init "transport select swd")

if("${BOARD_QUALIFIERS}" MATCHES "rp2350a")
  # The Cortex-M33 cores need the Arm target, the Hazard3 cores the RISC-V one.
  if(CONFIG_ARM)
    board_runner_args(openocd --cmd-pre-init "source [find target/rp2350.cfg]")
  else()
    board_runner_args(openocd --cmd-pre-init "source [find target/rp2350-riscv.cfg]")
  endif()
  board_runner_args(openocd --cmd-pre-init "set_adapter_speed_if_not_set 5000")

  board_runner_args(jlink "--device=RP2350_M33_0")
  board_runner_args(uf2 "--board-id=RP2350")
  board_runner_args(pyocd "--target=rp2350")
  board_runner_args(probe-rs "--chip=RP235x")
else()
  board_runner_args(openocd --cmd-pre-init "source [find target/rp2040.cfg]")
  board_runner_args(openocd --cmd-pre-init "set_adapter_speed_if_not_set 2000")

  board_runner_args(jlink "--device=RP2040_M0_0")
  board_runner_args(uf2 "--board-id=RPI-RP2")
  board_runner_args(pyocd "--target=rp2040")
endif()

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/uf2.board.cmake)
include(${ZEPHYR_BASE}/boards/common/blackmagicprobe.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
