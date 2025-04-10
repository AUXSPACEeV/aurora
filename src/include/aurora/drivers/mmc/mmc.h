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

#ifndef _MMC_H
#define _MMC_H

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"

/* SD/MMC version bits; 8 flags, 8 major, 8 minor, 8 change */
#define SD_VERSION_SD	(1U << 31)
#define MMC_VERSION_MMC	(1U << 30)

#define MAKE_SDMMC_VERSION(a, b, c)	\
	((((u32)(a)) << 16) | ((u32)(b) << 8) | (u32)(c))
#define MAKE_SD_VERSION(a, b, c)	\
	(SD_VERSION_SD | MAKE_SDMMC_VERSION(a, b, c))
#define MAKE_MMC_VERSION(a, b, c)	\
	(MMC_VERSION_MMC | MAKE_SDMMC_VERSION(a, b, c))

#define EXTRACT_SDMMC_MAJOR_VERSION(x)	\
	(((u32)(x) >> 16) & 0xff)
#define EXTRACT_SDMMC_MINOR_VERSION(x)	\
	(((u32)(x) >> 8) & 0xff)
#define EXTRACT_SDMMC_CHANGE_VERSION(x)	\
	((u32)(x) & 0xff)

#define SD_VERSION_3		MAKE_SD_VERSION(3, 0, 0)
#define SD_VERSION_2		MAKE_SD_VERSION(2, 0, 0)
#define SD_VERSION_1_0		MAKE_SD_VERSION(1, 0, 0)
#define SD_VERSION_1_10		MAKE_SD_VERSION(1, 10, 0)

#define MMC_VERSION_UNKNOWN	MAKE_MMC_VERSION(0, 0, 0)
#define MMC_VERSION_1_2		MAKE_MMC_VERSION(1, 2, 0)
#define MMC_VERSION_1_4		MAKE_MMC_VERSION(1, 4, 0)
#define MMC_VERSION_2_2		MAKE_MMC_VERSION(2, 2, 0)
#define MMC_VERSION_3		MAKE_MMC_VERSION(3, 0, 0)
#define MMC_VERSION_4		MAKE_MMC_VERSION(4, 0, 0)
#define MMC_VERSION_4_1		MAKE_MMC_VERSION(4, 1, 0)
#define MMC_VERSION_4_2		MAKE_MMC_VERSION(4, 2, 0)
#define MMC_VERSION_4_3		MAKE_MMC_VERSION(4, 3, 0)
#define MMC_VERSION_4_4		MAKE_MMC_VERSION(4, 4, 0)
#define MMC_VERSION_4_41	MAKE_MMC_VERSION(4, 4, 1)
#define MMC_VERSION_4_5		MAKE_MMC_VERSION(4, 5, 0)
#define MMC_VERSION_5_0		MAKE_MMC_VERSION(5, 0, 0)
#define MMC_VERSION_5_1		MAKE_MMC_VERSION(5, 1, 0)

#define MMC_CAP(mode)		(1 << mode)
#define MMC_MODE_HS		(MMC_CAP(MMC_HS) | MMC_CAP(SD_HS))
#define MMC_MODE_HS_52MHz	MMC_CAP(MMC_HS_52)
#define MMC_MODE_DDR_52MHz	MMC_CAP(MMC_DDR_52)
#define MMC_MODE_HS200		MMC_CAP(MMC_HS_200)
#define MMC_MODE_HS400		MMC_CAP(MMC_HS_400)
#define MMC_MODE_HS400_ES	MMC_CAP(MMC_HS_400_ES)

#define MMC_CAP_NONREMOVABLE	BIT(14)
#define MMC_CAP_NEEDS_POLL	BIT(15)
#define MMC_CAP_CD_ACTIVE_HIGH  BIT(16)

#define MMC_MODE_8BIT		BIT(30)
#define MMC_MODE_4BIT		BIT(29)
#define MMC_MODE_1BIT		BIT(28)
#define MMC_MODE_SPI		BIT(27)

#define SD_DATA_4BIT	0x00040000

#define IS_SD(x)	((x)->version & SD_VERSION_SD)
#define IS_MMC(x)	((x)->version & MMC_VERSION_MMC)

#define MMC_DATA_READ		1
#define MMC_DATA_WRITE		2

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

typedef enum mmc_response {
    MMC_RESP_R1,
    MMC_RESP_R1b,
    MMC_RESP_R2,
    MMC_RESP_R3,
    MMC_RESP_R6,
    MMC_RESP_R7,
    MMC_RESP_NONE,
} mmc_response_t;

struct mmc_dev {
    char *name;
    uint32_t version;
    uint32_t blksize;
    uint32_t num_blocks;
    bool initialized;

    void *priv;
};

struct mmc_ops {
    int (*probe)(struct mmc_dev *dev);
    int (*blk_read)(struct mmc_dev *dev, uint blk, uint8_t *buf, const size_t len);
    int (*blk_write)
        (struct mmc_dev *dev, uint blk, const uint8_t *buf, const size_t len);
    int (*blk_erase)(struct mmc_dev *dev, uint32_t addr);
};

struct mmc_drv {
    struct mmc_dev *dev;
    struct mmc_ops *ops;
};

size_t mmc_get_resp_size(mmc_response_t resp_type);

#endif /* _MMC_H */
