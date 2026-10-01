/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ABBY_DOWNLINK_H_
#define ABBY_DOWNLINK_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Counters for the LoRa radio.
 *
 * Which half is populated depends on the role: the transmit counters in
 * the relay role, the receive counters in the receive role. The other
 * half stays zero.
 */
struct downlink_stats {
	/* Relay role (transmitting). */
	uint32_t sent;        /**< Frames transmitted. */
	uint32_t tx_errs;     /**< lora_send() failures. */
	uint32_t superseded;  /**< Frames dropped because a newer one arrived. */
	uint32_t oversize;    /**< Frames too large for one LoRa packet. */
	uint32_t last_air_ms; /**< Airtime of the most recent transmission. */

	/* Receive role. */
	uint32_t received;    /**< Packets accepted as valid frames. */
	uint32_t rejected;    /**< Packets failing magic, length or CRC. */
	uint32_t rx_errs;     /**< lora_recv() failures. */
	int16_t  last_rssi;   /**< RSSI of the last accepted frame, dBm. */
	int8_t   last_snr;    /**< SNR of the last accepted frame, dB. */
};

/**
 * @brief Configure the radio for this role.
 *
 * In the relay role this readies the radio for transmission. In the
 * receive role it also starts the receive worker, so call it only once
 * the UART sink is up.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the radio is not ready.
 * @retval <0 as returned by lora_config().
 */
int downlink_init(void);

/**
 * @brief Hand a complete, CRC-checked frame to the radio.
 *
 * Relay role only. Never blocks. Only the newest frame is kept: if one
 * is already waiting it is replaced, because a telemetry downlink wants
 * the freshest state rather than a backlog.
 *
 * @param frame Frame bytes, starting at magic0.
 * @param len   Frame length in bytes.
 *
 * @retval 0 on success.
 * @retval -EMSGSIZE if @p len exceeds one LoRa packet.
 */
int downlink_submit(const uint8_t *frame, size_t len);

/** @brief Snapshot the radio counters. */
void downlink_get_stats(struct downlink_stats *out);

#endif /* ABBY_DOWNLINK_H_ */
