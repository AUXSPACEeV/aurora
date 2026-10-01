/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ABBY_LINK_H_
#define ABBY_LINK_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Counters for the AUX-Stack connector UART.
 *
 * Which half is populated depends on the role: the receive counters in
 * the relay role, @c sent in the receive role.
 */
struct link_stats {
	uint32_t frames;    /**< Frames accepted (magic and CRC good). */
	uint32_t crc_errs;  /**< Frames discarded on a CRC mismatch. */
	uint32_t overruns;  /**< Bytes dropped because the ring buffer was full. */
	uint32_t sent;      /**< Frames written out to the UART. */
};

/**
 * @brief Bring up the stack UART.
 *
 * In the relay role this also installs the receive interrupt and starts
 * the frame parser, so call it only once the radio is up.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the UART is not ready.
 */
int link_init(void);

/**
 * @brief Write a complete frame out to the stack UART.
 *
 * Receive role only. Blocks for the frame's transmission time - about
 * 6 ms for a 70-byte frame at 115200 baud - and serialises against
 * other callers so a frame is never interleaved with another.
 *
 * @param frame Frame bytes, starting at magic0.
 * @param len   Frame length in bytes.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p frame is NULL or @p len is zero.
 */
int link_send(const uint8_t *frame, size_t len);

/** @brief Snapshot the link counters. */
void link_get_stats(struct link_stats *out);

#endif /* ABBY_LINK_H_ */
