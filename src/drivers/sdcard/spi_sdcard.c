/**
* @file spi_sdcard.c
* @brief Sources for SDCard I/O via SPI
* @note This file contains the function implementations for SDCard I/O via SPI.
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*/
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#include <aurora/drivers/spi_flash.h>
#include <aurora/drivers/sdcard_io.h>


