/**
* @file spi_flash.h
* @brief SPI Flash library
* @note This file contains the function prototypes and definitions for
* SPI flashes.
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*/

#ifndef SPI_FLASH_H
#define SPI_FLASH_H

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define FLASH_PAGE_SIZE        256
#define FLASH_SECTOR_SIZE      4096

void spi_flash_read (spi_inst_t *spi, uint cs_pin, uint32_t addr, uint8_t *buf, size_t len);

void spi_flash_write_enable (spi_inst_t *spi, uint cs_pin);

void spi_flash_wait_done (spi_inst_t *spi, uint cs_pin);

void spi_flash_sector_erase (spi_inst_t *spi, uint cs_pin, uint32_t addr);

void spi_flash_page_program (spi_inst_t *spi, uint cs_pin, uint32_t addr, uint8_t data[]);

void spi_flash_dbg_printbuf (uint8_t buf[FLASH_PAGE_SIZE]);

#endif /* SPI_FLASH_H */
