# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: MIT

zephyr_library()

# Vendored CEVA SH-2 / SHTP protocol library.
file(GLOB sh2_sources sh2/*.c)
zephyr_library_sources(${sh2_sources})

zephyr_library_sources(bno08x.c)
zephyr_library_sources(bno08x_spi.c)

# The vendored sh2 sources are third-party and not warning-clean under the
# Zephyr flag set; keep them from failing the build on -Werror configs.
zephyr_library_compile_options(-Wno-unused-parameter)
