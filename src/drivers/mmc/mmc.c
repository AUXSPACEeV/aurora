/**
 * @file mmc.c
 * @brief Sources for generic MMC/SD I/O
 * @note This file contains the source code for generic MMC/SD I/O.
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */
#include "pico/stdlib.h"

#include <aurora/drivers/mmc/mmc.h>

size_t mmc_get_resp_size(mmc_response_t resp_type)
{
    switch (resp_type) {
        case MMC_RESP_R1:
        case MMC_RESP_R1b:
            return 1;
        case MMC_RESP_R3:
        case MMC_RESP_R6:
        case MMC_RESP_R7:
            return 4;
        case MMC_RESP_R2:
            return 2;
        default:
            return 0;
    }
}

/*----------------------------------------------------------------------------*/

mmc_response_t mmc_cmd_get_resp_type(uint8_t cmd)
{
    // TODO: Check all commands
    switch (cmd) {
        case MMC_CMD_SEND_OP_COND:
        case MMC_CMD_SEND_STATUS:
        case MMC_CMD_SET_BLOCKLEN:
        case MMC_CMD_READ_SINGLE_BLOCK:
        case MMC_CMD_READ_MULTIPLE_BLOCK:
        case MMC_CMD_WRITE_SINGLE_BLOCK:
        case MMC_CMD_WRITE_MULTIPLE_BLOCK:
        case MMC_CMD_APP_CMD:
        case MMC_CMD_SPI_READ_OCR:
            return MMC_RESP_R1;
        case MMC_CMD_SELECT_CARD:
        case MMC_CMD_STOP_TRANSMISSION:
        case MMC_CMD_ERASE:
            return MMC_RESP_R1b;
        case MMC_CMD_ALL_SEND_CID:
        case MMC_CMD_SEND_CSD:
        case MMC_CMD_SEND_CID:
            return MMC_RESP_R2;
        case MMC_CMD_SET_RELATIVE_ADDR:
            return MMC_RESP_R6;
        case MMC_CMD_SEND_EXT_CSD:
            return MMC_RESP_R7;
        case MMC_CMD_GO_IDLE_STATE:
        case MMC_CMD_SET_DSR:
        default:
            return MMC_RESP_NONE;
    }
}

/* [] END OF FILE */