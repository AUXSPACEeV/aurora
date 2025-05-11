/**
* @file mmc.h
* @brief MMC library
* @note This file contains the function prototypes and definitions for MMCs.
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*
* @note This file is based on u-boot's mmc header file.
* @note The original file can be found at:
* https://source.denx.de/u-boot/u-boot/-/blob/master/include/mmc.h
* @note The original file is licensed under the GNU General Public License v2.0.
*/

#pragma once

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"

#define MMC_CMD_GO_IDLE_STATE		0
#define MMC_CMD_SEND_OP_COND		1
#define MMC_CMD_ALL_SEND_CID		2
#define MMC_CMD_SET_RELATIVE_ADDR	3
#define MMC_CMD_SET_DSR			4
#define MMC_CMD_SWITCH			6
#define MMC_CMD_SELECT_CARD		7
#define MMC_CMD_SEND_EXT_CSD		8
#define MMC_CMD_SEND_CSD		9
#define MMC_CMD_SEND_CID		10
#define MMC_CMD_STOP_TRANSMISSION	12
#define MMC_CMD_SEND_STATUS		13
#define MMC_CMD_SET_BLOCKLEN		16
#define MMC_CMD_READ_SINGLE_BLOCK	17
#define MMC_CMD_READ_MULTIPLE_BLOCK	18
#define MMC_CMD_SEND_TUNING_BLOCK		19
#define MMC_CMD_SEND_TUNING_BLOCK_HS200	21
#define MMC_CMD_SET_BLOCK_COUNT         23
#define MMC_CMD_WRITE_SINGLE_BLOCK	24
#define MMC_CMD_WRITE_MULTIPLE_BLOCK	25
#define MMC_CMD_ERASE_GROUP_START	35
#define MMC_CMD_ERASE_GROUP_END		36
#define MMC_CMD_ERASE			38
#define MMC_CMD_APP_CMD			55
#define MMC_CMD_SPI_READ_OCR		58
#define MMC_CMD_SPI_CRC_ON_OFF		59
#define MMC_CMD_RES_MAN			62

#define MMC_CMD62_ARG1			0xefac62ec
#define MMC_CMD62_ARG2			0xcbaea7

#define SD_CMD_SEND_RELATIVE_ADDR	3
#define SD_CMD_SWITCH_FUNC		6
#define SD_CMD_SEND_IF_COND		8
#define SD_CMD_SWITCH_UHS18V		11

#define SD_CMD_APP_SET_BUS_WIDTH	6
#define SD_CMD_APP_SD_STATUS		13
#define SD_CMD_ERASE_WR_BLK_START	32
#define SD_CMD_ERASE_WR_BLK_END		33
#define SD_CMD_APP_SEND_OP_COND		41
#define SD_CMD_APP_SEND_SCR		51

#define BLOCK_SIZE_SD 512  /*!< Block size supported for SD card is 512 bytes */

/**
 * @brief MMC card types / versions
 *
 * @note The types / versions are based on the SD/MMC specification.
 *
 * @ref https://users.ece.utexas.edu/~valvano/EE345M/SD_Physical_Layer_Spec.pdf
 */
typedef enum mmc_type {
    SD_CARD_TYPE_SD1,
    SD_CARD_TYPE_SD2,
    SD_CARD_TYPE_SDHC,
} mmc_type_t;

/**
 * @brief MMC response types
 *
 * @note The response types are based on the SD/MMC specification.
 *
 * @ref https://users.ece.utexas.edu/~valvano/EE345M/SD_Physical_Layer_Spec.pdf
 */
typedef enum mmc_response {
    MMC_RESP_R1,
    MMC_RESP_R1b,
    MMC_RESP_R2,
    MMC_RESP_R3,
    MMC_RESP_R6,
    MMC_RESP_R7,
    MMC_RESP_NONE,
} mmc_response_t;

/**
 * @brief MMC device structure
 *
 * @param name: Name of the device
 * @param version: Version of the MMC device
 * @param blksize: Block size of the device
 * @param num_blocks: Number of blocks on the device
 * @param initialized: Flag to check if the device is initialized
 * @param priv: Pointer to private data
 */
struct mmc_dev {
    char *name;
    mmc_type_t version;
    uint32_t blksize;
    uint32_t num_blocks;
    bool initialized;
    void *priv;
};

/**
 * @brief MMC driver functions
 *
 * @param probe: Probe function to initialize the device
 * @param blk_read: Function to read a block of data
 * @param blk_write: Function to write blocks of data
 * @param blk_erase: Function to erase blocks of data
 * @param generate_info: Function to generate device information
 * @param n_sectors: Function to get the number of sectors on the mmc device
 */
struct mmc_ops {
    int (*probe)(struct mmc_dev *dev);
    int (*blk_read)(struct mmc_dev *dev, uint blk, uint8_t *buf, const size_t len);
    int (*blk_write)
        (struct mmc_dev *dev, uint blk, const uint8_t *buf, const size_t len);
    int (*blk_erase)(struct mmc_dev *dev, uint32_t addr);
    int (*generate_info)(struct mmc_dev *dev);
    ssize_t (*n_sectors)(struct mmc_dev *dev);
};

/**
 * @brief MMC driver structure
 *
 * @param dev: Pointer to the MMC device structure
 * @param ops: Pointer to the MMC functions
 */
struct mmc_drv {
    struct mmc_dev *dev;
    struct mmc_ops *ops;
};

/**
 * @brief Get the response size for a given MMC response type
 *
 * @param resp_type: The response type
 *
 * @return The size of the response in bytes
 */
size_t mmc_get_resp_size(mmc_response_t resp_type);

/**
 * @brief Get the response type for a given MMC command
 *
 * @param cmd: The command number
 *
 * @return The response type for the command
 */
mmc_response_t mmc_cmd_get_resp_type(uint8_t cmd);

/* [] END OF FILE */