/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TELEMETRY_LOGGER_LINK_H_
#define TELEMETRY_LOGGER_LINK_H_

#include <stdint.h>

/** @brief Counters for the AUX-Stack connector UART. */
struct link_stats {
	uint32_t frames;    /**< Frames accepted (magic and CRC good). */
	uint32_t crc_errs;  /**< Frames discarded on a CRC mismatch. */
	uint32_t overruns;  /**< Bytes dropped because the ring buffer was full. */
};

/**
 * @brief Bring up the stack UART and start parsing frames.
 *
 * Every frame that passes its CRC goes to the recorder, so call it only
 * once the recorder is up.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the UART is not ready.
 */
int link_init(void);

/** @brief Snapshot the link counters. */
void link_get_stats(struct link_stats *out);

#endif /* TELEMETRY_LOGGER_LINK_H_ */
