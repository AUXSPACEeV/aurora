/**
* @file spi_sd.h
* @brief SPI micro SD Card library
* @note This file contains the function prototypes and definitions for
* SPI micro SD (uSD) Cards.
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*/

#ifndef SPI_SDCARD_H
#define SPI_SDCARD_H

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define SPI_SDCARD_PAGE_SIZE        256
#define SPI_SDCARD_SECTOR_SIZE      4096

int spi_sd_init (spi_inst_t *spi, uint cs_pin);

void spi_sd_read (spi_inst_t *spi, uint cs_pin, uint32_t addr, uint8_t *buf, size_t len);

void spi_sd_write_enable (spi_inst_t *spi, uint cs_pin);

void spi_sd_wait_done (spi_inst_t *spi, uint cs_pin);

void spi_sd_sector_erase (spi_inst_t *spi, uint cs_pin, uint32_t addr);

void spi_sd_page_program (spi_inst_t *spi, uint cs_pin, uint32_t addr, uint8_t data[]);

void spi_sd_dbg_printbuf (uint8_t buf[SPI_SDCARD_PAGE_SIZE]);

#endif /* SPI_SDCARD_H */
