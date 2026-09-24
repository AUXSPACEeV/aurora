# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()


if(CONFIG_CAN)
  zephyr_library_sources(can.c)
endif()



