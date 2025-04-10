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

size_t mmc_get_resp_size(mmc_response_t resp_type) {
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
