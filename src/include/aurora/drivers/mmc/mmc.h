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

#include <aurora/macros.h>

/* SD/MMC version bits; 8 flags, 8 major, 8 minor, 8 change */
#define SD_VERSION_SD	(1U << 31)
#define MMC_VERSION_MMC	(1U << 30)

#define MAKE_SDMMC_VERSION(a, b, c)	\
	((((uint32_t)(a)) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(c))
#define MAKE_SD_VERSION(a, b, c)	\
	(SD_VERSION_SD | MAKE_SDMMC_VERSION(a, b, c))
#define MAKE_MMC_VERSION(a, b, c)	\
	(MMC_VERSION_MMC | MAKE_SDMMC_VERSION(a, b, c))

#define EXTRACT_SDMMC_MAJOR_VERSION(x)	\
	(((uint32_t)(x) >> 16) & 0xff)
#define EXTRACT_SDMMC_MINOR_VERSION(x)	\
	(((uint32_t)(x) >> 8) & 0xff)
#define EXTRACT_SDMMC_CHANGE_VERSION(x)	\
	((uint32_t)(x) & 0xff)

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

#define SD_DATA_4BIT	0x00040000

#define IS_SD(mmc)	((mmc)->version & SD_VERSION_SD)
#define IS_MMC(mmc)	((mmc)->version & MMC_VERSION_MMC)

/*
 * EXT_CSD fields
 */
#define EXT_CSD_BOOT_SIZE_MULT_MICRON	125	/* R/W, vendor specific field */
#define EXT_CSD_ENH_START_ADDR		136	/* R/W */
#define EXT_CSD_ENH_SIZE_MULT		140	/* R/W */
#define EXT_CSD_GP_SIZE_MULT		143	/* R/W */
#define EXT_CSD_PARTITION_SETTING	155	/* R/W */
#define EXT_CSD_PARTITIONS_ATTRIBUTE	156	/* R/W */
#define EXT_CSD_MAX_ENH_SIZE_MULT	157	/* R */
#define EXT_CSD_PARTITIONING_SUPPORT	160	/* RO */
#define EXT_CSD_RST_N_FUNCTION		162	/* R/W */
#define EXT_CSD_BKOPS_EN		163	/* R/W & R/W/E */
#define EXT_CSD_WR_REL_PARAM		166	/* R */
#define EXT_CSD_WR_REL_SET		167	/* R/W */
#define EXT_CSD_RPMB_MULT		168	/* RO */
#define EXT_CSD_USER_WP			171	/* R/W & R/W/C_P & R/W/E_P */
#define EXT_CSD_BOOT_WP			173	/* R/W & R/W/C_P */
#define EXT_CSD_BOOT_WP_STATUS		174	/* R */
#define EXT_CSD_ERASE_GROUP_DEF		175	/* R/W */
#define EXT_CSD_BOOT_BUS_WIDTH		177
#define EXT_CSD_PART_CONF		179	/* R/W */
#define EXT_CSD_BUS_WIDTH		183	/* R/W */
#define EXT_CSD_STROBE_SUPPORT		184	/* R/W */
#define EXT_CSD_HS_TIMING		185	/* R/W */
#define EXT_CSD_REV			192	/* RO */
#define EXT_CSD_CARD_TYPE		196	/* RO */
#define EXT_CSD_PART_SWITCH_TIME	199	/* RO */
#define EXT_CSD_SEC_CNT			212	/* RO, 4 bytes */
#define EXT_CSD_HC_WP_GRP_SIZE		221	/* RO */
#define EXT_CSD_HC_ERASE_GRP_SIZE	224	/* RO */
#define EXT_CSD_BOOT_MULT		226	/* RO */
#define EXT_CSD_SEC_FEATURE		231	/* RO */
#define EXT_CSD_GENERIC_CMD6_TIME       248     /* RO */
#define EXT_CSD_BKOPS_SUPPORT		502	/* RO */

/*
 * EXT_CSD field definitions
 */

#define EXT_CSD_CMD_SET_NORMAL		(1 << 0)
#define EXT_CSD_CMD_SET_SECURE		(1 << 1)
#define EXT_CSD_CMD_SET_CPSECURE	(1 << 2)

#define EXT_CSD_CARD_TYPE_26	(1 << 0)	/* Card can run at 26MHz */
#define EXT_CSD_CARD_TYPE_52	(1 << 1)	/* Card can run at 52MHz */
#define EXT_CSD_CARD_TYPE_DDR_1_8V	(1 << 2)
#define EXT_CSD_CARD_TYPE_DDR_1_2V	(1 << 3)
#define EXT_CSD_CARD_TYPE_DDR_52	(EXT_CSD_CARD_TYPE_DDR_1_8V \
					| EXT_CSD_CARD_TYPE_DDR_1_2V)

#define EXT_CSD_CARD_TYPE_HS200_1_8V	BIT(4)	/* Card can run at 200MHz */
						/* SDR mode @1.8V I/O */
#define EXT_CSD_CARD_TYPE_HS200_1_2V	BIT(5)	/* Card can run at 200MHz */
						/* SDR mode @1.2V I/O */
#define EXT_CSD_CARD_TYPE_HS200		(EXT_CSD_CARD_TYPE_HS200_1_8V | \
					 EXT_CSD_CARD_TYPE_HS200_1_2V)
#define EXT_CSD_CARD_TYPE_HS400_1_8V	BIT(6)
#define EXT_CSD_CARD_TYPE_HS400_1_2V	BIT(7)
#define EXT_CSD_CARD_TYPE_HS400		(EXT_CSD_CARD_TYPE_HS400_1_8V | \
					 EXT_CSD_CARD_TYPE_HS400_1_2V)

#define EXT_CSD_BUS_WIDTH_1	0	/* Card is in 1 bit mode */
#define EXT_CSD_BUS_WIDTH_4	1	/* Card is in 4 bit mode */
#define EXT_CSD_BUS_WIDTH_8	2	/* Card is in 8 bit mode */
#define EXT_CSD_DDR_BUS_WIDTH_4	5	/* Card is in 4 bit DDR mode */
#define EXT_CSD_DDR_BUS_WIDTH_8	6	/* Card is in 8 bit DDR mode */
#define EXT_CSD_DDR_FLAG	BIT(2)	/* Flag for DDR mode */
#define EXT_CSD_BUS_WIDTH_STROBE BIT(7)	/* Enhanced strobe mode */

#define EXT_CSD_TIMING_LEGACY	0	/* no high speed */
#define EXT_CSD_TIMING_HS	1	/* HS */
#define EXT_CSD_TIMING_HS200	2	/* HS200 */
#define EXT_CSD_TIMING_HS400	3	/* HS400 */
#define EXT_CSD_DRV_STR_SHIFT	4	/* Driver Strength shift */

#define EXT_CSD_BOOT_ACK_ENABLE			(1 << 6)
#define EXT_CSD_BOOT_PARTITION_ENABLE		(1 << 3)
#define EXT_CSD_PARTITION_ACCESS_ENABLE		(1 << 0)
#define EXT_CSD_PARTITION_ACCESS_DISABLE	(0 << 0)

#define EXT_CSD_BOOT_ACK(x)		(x << 6)
#define EXT_CSD_BOOT_PART_NUM(x)	(x << 3)
#define EXT_CSD_PARTITION_ACCESS(x)	(x << 0)

#define EXT_CSD_EXTRACT_BOOT_ACK(x)		(((x) >> 6) & 0x1)
#define EXT_CSD_EXTRACT_BOOT_PART(x)		(((x) >> 3) & 0x7)
#define EXT_CSD_EXTRACT_PARTITION_ACCESS(x)	((x) & 0x7)

#define EXT_CSD_BOOT_BUS_WIDTH_MODE(x)	(x << 3)
#define EXT_CSD_BOOT_BUS_WIDTH_RESET(x)	(x << 2)
#define EXT_CSD_BOOT_BUS_WIDTH_WIDTH(x)	(x)

#define EXT_CSD_PARTITION_SETTING_COMPLETED	(1 << 0)

#define EXT_CSD_ENH_USR		(1 << 0)	/* user data area is enhanced */
#define EXT_CSD_ENH_GP(x)	(1 << ((x)+1))	/* GP part (x+1) is enhanced */

#define EXT_CSD_HS_CTRL_REL	(1 << 0)	/* host controlled WR_REL_SET */

#define EXT_CSD_BOOT_WP_B_SEC_WP_SEL	(0x80)	/* enable partition selector */
#define EXT_CSD_BOOT_WP_B_PWR_WP_SEC_SEL (0x02)	/* partition selector to protect */
#define EXT_CSD_BOOT_WP_B_PWR_WP_EN	(0x01)	/* power-on write-protect */

#define EXT_CSD_WR_DATA_REL_USR		(1 << 0)	/* user data area WR_REL */
#define EXT_CSD_WR_DATA_REL_GP(x)	(1 << ((x)+1))	/* GP part (x+1) WR_REL */

#define EXT_CSD_SEC_FEATURE_TRIM_EN	(1 << 4) /* Support secure & insecure trim */

#define R1_ILLEGAL_COMMAND		(1 << 22)
#define R1_APP_CMD			(1 << 5)

#define MMC_RSP_PRESENT (1 << 0)
#define MMC_RSP_136	(1 << 1)		/* 136 bit response */
#define MMC_RSP_CRC	(1 << 2)		/* expect valid crc */
#define MMC_RSP_BUSY	(1 << 3)		/* card may send busy */
#define MMC_RSP_OPCODE	(1 << 4)		/* response contains opcode */

#define MMC_RSP_NONE	(0)
#define MMC_RSP_R1	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R1b	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE| \
			MMC_RSP_BUSY)
#define MMC_RSP_R2	(MMC_RSP_PRESENT|MMC_RSP_136|MMC_RSP_CRC)
#define MMC_RSP_R3	(MMC_RSP_PRESENT)
#define MMC_RSP_R4	(MMC_RSP_PRESENT)
#define MMC_RSP_R5	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R6	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R7	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)

#define MMCPART_NOAVAILABLE	(0xff)
#define PART_ACCESS_MASK	(0x7)
#define PART_SUPPORT		(0x1)
#define ENHNCD_SUPPORT		(0x2)
#define PART_ENH_ATTRIB		(0x1f)

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

#define SD_COMMAND_TIMEOUT  2000

#define MMC_VDD_165_195		0x00000080	/* VDD voltage 1.65 - 1.95 */
#define MMC_VDD_20_21		0x00000100	/* VDD voltage 2.0 ~ 2.1 */
#define MMC_VDD_21_22		0x00000200	/* VDD voltage 2.1 ~ 2.2 */
#define MMC_VDD_22_23		0x00000400	/* VDD voltage 2.2 ~ 2.3 */
#define MMC_VDD_23_24		0x00000800	/* VDD voltage 2.3 ~ 2.4 */
#define MMC_VDD_24_25		0x00001000	/* VDD voltage 2.4 ~ 2.5 */
#define MMC_VDD_25_26		0x00002000	/* VDD voltage 2.5 ~ 2.6 */
#define MMC_VDD_26_27		0x00004000	/* VDD voltage 2.6 ~ 2.7 */
#define MMC_VDD_27_28		0x00008000	/* VDD voltage 2.7 ~ 2.8 */
#define MMC_VDD_28_29		0x00010000	/* VDD voltage 2.8 ~ 2.9 */
#define MMC_VDD_29_30		0x00020000	/* VDD voltage 2.9 ~ 3.0 */
#define MMC_VDD_30_31		0x00040000	/* VDD voltage 3.0 ~ 3.1 */
#define MMC_VDD_31_32		0x00080000	/* VDD voltage 3.1 ~ 3.2 */
#define MMC_VDD_32_33		0x00100000	/* VDD voltage 3.2 ~ 3.3 */
#define MMC_VDD_33_34		0x00200000	/* VDD voltage 3.3 ~ 3.4 */
#define MMC_VDD_34_35		0x00400000	/* VDD voltage 3.4 ~ 3.5 */
#define MMC_VDD_35_36		0x00800000	/* VDD voltage 3.5 ~ 3.6 */

#define MMC_SWITCH_MODE_CMD_SET		0x00 /* Change the command set */
#define MMC_SWITCH_MODE_SET_BITS	0x01 /* Set bits in EXT_CSD byte
						addressed by index which are
						1 in value field */
#define MMC_SWITCH_MODE_CLEAR_BITS	0x02 /* Clear bits in EXT_CSD byte
						addressed by index, which are
						1 in value field */
#define MMC_SWITCH_MODE_WRITE_BYTE	0x03 /* Set target byte to value */

#define SD_SWITCH_CHECK		0
#define SD_SWITCH_SWITCH	1

#define OCR_BUSY		0x80000000
#define OCR_HCS			0x40000000
#define OCR_S18R		0x1000000
#define OCR_VOLTAGE_MASK	0x007FFF80
#define OCR_ACCESS_MODE		0x60000000

#define MMC_STATUS_MASK		(~0x0206BF7F)
#define MMC_STATUS_SWITCH_ERROR	(1 << 7)
#define MMC_STATUS_RDY_FOR_DATA (1 << 8)
#define MMC_STATUS_CURR_STATE	(0xf << 9)
#define MMC_STATUS_ERROR	(1 << 19)

#define MMC_STATE_PRG		(7 << 9)
#define MMC_STATE_TRANS		(4 << 9)

#define MMC_DATA_READ		1
#define MMC_DATA_WRITE		2

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

#define MMC_QUIRK_RETRY_SEND_CID	BIT(0)
#define MMC_QUIRK_RETRY_SET_BLOCKLEN	BIT(1)
#define MMC_QUIRK_RETRY_APP_CMD	BIT(2)

/* Maximum block size for MMC */
#define MMC_MAX_BLOCK_LEN	512

/* Minimum partition switch timeout in units of 10-milliseconds */
#define MMC_MIN_PART_SWITCH_TIME	30 /* 300 ms */

/* SCR definitions in different words */
#define SD_HIGHSPEED_BUSY	0x00020000
#define SD_HIGHSPEED_SUPPORTED	0x00020000

#define UHS_CAPS (MMC_CAP(UHS_SDR12) | MMC_CAP(UHS_SDR25) | \
		  MMC_CAP(UHS_SDR50) | MMC_CAP(UHS_SDR104) | \
		  MMC_CAP(UHS_DDR50))

struct sd_ssr {
	unsigned int au;		/* In sectors */
	unsigned int erase_timeout;	/* In milliseconds */
	unsigned int erase_offset;	/* In milliseconds */
};

enum mmc_voltage {
	MMC_SIGNAL_VOLTAGE_000 = 0,
	MMC_SIGNAL_VOLTAGE_120 = 1,
	MMC_SIGNAL_VOLTAGE_180 = 2,
	MMC_SIGNAL_VOLTAGE_330 = 4,
};

#define MMC_ALL_SIGNAL_VOLTAGE (MMC_SIGNAL_VOLTAGE_120 |\
				MMC_SIGNAL_VOLTAGE_180 |\
				MMC_SIGNAL_VOLTAGE_330)

enum bus_mode {
	MMC_LEGACY,
	MMC_HS,
	SD_HS,
	MMC_HS_52,
	MMC_DDR_52,
	UHS_SDR12,
	UHS_SDR25,
	UHS_SDR50,
	UHS_DDR50,
	UHS_SDR104,
	MMC_HS_200,
	MMC_HS_400,
	MMC_HS_400_ES,
	MMC_MODES_END,
};

/**
 * @brief MMC Card command
 * 
 * @param cmdidx: command index
 * 
 * @param resp_type: mmc response type
 * 
 * @param cmdarg: cmd argument
 * 
 * @param response: response after SDHCI specification
 */
struct mmc_cmd {
	uint32_t cmdidx;
	uint32_t resp_type;
	uint32_t cmdarg;
	uint32_t response[4];
};

struct mmc_data {
	union {
		char *dest;
		const char *src; /* src buffers don't get written to */
	};
	uint32_t flags;
	uint32_t blocks;
	uint32_t blocksize;
};

/**
 * @brief MMC device structure
 *
 * @param name: Name of the device
 * @param version: Version of the MMC device
 * @param blksize: Block size of the device
 * @param num_blocks: Number of blocks on the device
 * @param initialized: Flag to check if the device is initialized
 * @param host_caps: Host capability flags
 * @param priv: Pointer to private data
 */
struct mmc_dev {
    char *name;
    uint32_t version;
    uint32_t blksize;
    uint64_t num_blocks;
    bool initialized;
    bool init_in_progress;
    uint32_t host_caps;
    uint32_t card_caps;
    uint32_t quirks;
    uint8_t gen_cmd6_time;	/* units: 10 ms */
	uint8_t part_switch_time;	/* units: 10 ms */
	unsigned short rca;
	char op_cond_pending;	/* 1 if we are waiting on an op_cond command */
	uint32_t ocr;
	uint32_t scr[2];
    uint32_t csd[4];
	uint32_t cid[4];
	uint32_t voltages;
	uint32_t f_min;
	uint32_t f_max;
	uint32_t b_max;
    uint32_t clock;
	enum mmc_voltage signal_voltage;
	bool clk_disable; /* true if the clock can be turned off */
	enum bus_mode user_speed_mode; /* input speed mode from user */
	int high_capacity;
    uint32_t tran_speed;
	uint32_t legacy_speed; /* speed for the legacy mode provided by the card */
    enum bus_mode selected_mode; /* mode currently used */
    enum bus_mode best_mode; /* mode currently used */
	struct sd_ssr	ssr;	/* SD status register */
	int ddr_mode;
	uint32_t dsr;
	uint32_t dsr_imp;
    uint32_t read_bl_len;
	uint32_t write_bl_len;
	uint32_t erase_grp_size;	/* in 512-byte sectors */
	uint64_t capacity;
	uint8_t part_config;
	uint8_t *ext_csd;
    uint8_t part_support;
	uint8_t part_attr;
	uint8_t wr_rel_set;
	uint8_t part_config;
	uint32_t hc_wp_grp_size;	/* in 512-byte sectors */
	bool can_trim;
	uint32_t cardtype;		/* cardtype read from the MMC */
	uint32_t bus_width;
    void *priv;
};

/**
 * @brief MMC driver functions
 *
 * @param probe: Probe function to initialize the device
 * @param blk_read: Function to read blocks of data
 * @param blk_write: Function to write blocks of data
 * @param blk_erase: Function to erase blocks of data
 * @param generate_info: Function to generate device information
 * @param n_sectors: Function to get the number of sectors on the mmc device
 */
struct mmc_ops {
    /**
     * @brief Probe function to initialize the device
     * 
     * @param dev: mmc device to probe
     * @return: Error code on failure
     */
    int (*deferred_probe)(struct mmc_dev *dev);

    /**
     * @brief Send a command to the card
     * 
     * @param dev: mmc device to send command to
     * @param cmd: Command to send
     * @param data: Additional data to send/receive
     * @return: Error code on failure
     */
    int (*send_cmd)(struct mmc_dev *dev, struct mmc_cmd *cmd, struct mmc_data *data);

	int (*wait_dat0)(struct mmc_dev *dev, int state, uint32_t timeout_us);

    /**
	 * @brief See whether a card is present
	 *
	 * @dev:	Device to check
	 * @return 0 if not present, 1 if present, -ve on error
	 */
	int (*get_cd)(struct mmc_dev *dev);

    int (*set_ios)(struct mmc_dev *dev);

	/**
	 * execute_tuning() - Start the tuning process
	 *
	 * @dev:	Device to start the tuning
	 * @opcode:	Command opcode to send
	 * @return 0 if OK, -ve on error
	 */
	int (*execute_tuning)(struct mmc_dev *dev, uint32_t opcode);
};

/**
 * @brief MMC driver structure
 *
 * @param dev: Pointer to the MMC device structure
 * @param ops: Pointer to the MMC functions
 */
struct mmc_drv {
    struct mmc_dev *dev;
    const struct mmc_ops *ops;
};

int mmc_send_cmd(struct mmc_drv *mmc, struct mmc_cmd *cmd, struct mmc_data *data);

#ifdef CONFIG_MMC_SPI
#define mmc_host_is_spi(mmc)	((mmc)->host_caps & MMC_MODE_SPI)
#else
#define mmc_host_is_spi(mmc)	0
#endif

static inline bool mmc_is_mode_ddr(enum bus_mode mode)
{
	if (mode == MMC_DDR_52)
		return true;
	else if (mode == UHS_DDR50)
		return true;
	else if (mode == MMC_HS_400)
		return true;
	else if (mode == MMC_HS_400_ES)
		return true;
	else
		return false;
}

/* [] END OF FILE */