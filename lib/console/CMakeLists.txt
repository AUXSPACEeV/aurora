# Copyright (c) 2025 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

if(CONFIG_USB_SERIAL)
    zephyr_library_sources(usb_serial.c)
endif()
