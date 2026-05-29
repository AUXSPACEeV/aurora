/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_TELEMETRY_HC12_INTERNAL_H_
#define AURORA_LIB_TELEMETRY_HC12_INTERNAL_H_

#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

/* The HC-12 always speaks 9600 baud while SET is held low, regardless
 * of the configured air-side rate. The AT helper switches the host
 * UART to this rate for the duration of an exchange and restores the
 * original on completion.
 */
#define HC12_AT_BAUD 9600

/* Time the HC-12 needs to enter / leave AT mode after a SET edge. */
#define HC12_SET_SETTLE_MS 80

/* Wire frame packet types. */
#define HC12_TYPE_SM_UPDATE 0x01

/** @brief HC-12 SM_UPDATE wire payload (little-endian, packed, 64 B). */
struct __packed hc12_sm_update_payload {
	uint32_t timestamp_ms;
	uint8_t  state;
	uint8_t  armed;
	int16_t  reserved;
	double   altitude;
	double   acceleration;
	double   accel_vert;
	double   velocity;
	double   orientation[3];
};

/**
 * @brief Build a complete HC-12 wire frame in @p buf.
 *
 * Layout (all multi-byte fields little-endian):
 *   buf[0]    magic0   = 0xA5
 *   buf[1]    magic1   = 0x5A
 *   buf[2]    type
 *   buf[3]    payload_len
 *   buf[4..]  payload  (payload_len bytes)
 *   buf[..]   CRC-16/CCITT (init 0xFFFF) over buf[2 .. 4+payload_len-1]
 *
 * @param buf          Output buffer.
 * @param buf_sz       Size of @p buf in bytes.
 * @param type         Packet type byte.
 * @param payload      Payload bytes.
 * @param payload_len  Length of @p payload (must fit in @p buf with the
 *                     6 bytes of header + CRC).
 *
 * @return Total frame length written, or 0 if @p buf is too small.
 */
size_t hc12_frame_finalise(uint8_t *buf, size_t buf_sz, uint8_t type,
			   const void *payload, uint8_t payload_len);

/** @brief UART device the backend talks to. Resolved from the binding. */
extern const struct device *const hc12_uart_dev;

/** @brief Serialises every byte sent on hc12_uart_dev. Held by the TX
 *  worker per frame and by the AT helper for the duration of one
 *  command exchange.
 */
extern struct k_mutex hc12_uart_lock;

#if defined(CONFIG_AURORA_TELEMETRY_HC12_AT)

/**
 * @brief Run one AT command synchronously.
 *
 * Drives SET low, switches the UART to 9600 baud, writes @p cmd
 * (no trailing terminator is added; pass the exact bytes the HC-12
 * expects), then reads bytes into @p resp until @p quiet_ms have
 * elapsed without a new byte or until the buffer fills. Restores the
 * original UART configuration and releases SET on every exit path.
 *
 * Refuses with -ENOTSUP if the SET pin is not wired in DT.
 *
 * @param cmd        Command bytes (e.g. "AT+RX").
 * @param cmd_len    Number of bytes in @p cmd.
 * @param resp       Buffer that receives the response, NUL-terminated.
 *                   May be NULL if @p resp_sz is 0.
 * @param resp_sz    Size of @p resp in bytes.
 * @param quiet_ms   Inactivity gap (ms) that terminates the read.
 * @param resp_out_len  Optional: receives the number of response
 *                      bytes actually written (excluding the NUL).
 *
 * @retval 0        Command sent (response, if any, captured).
 * @retval -EINVAL  cmd is NULL or cmd_len is 0.
 * @retval -ENOTSUP SET pin not wired.
 * @retval -EIO     UART configure failed.
 */
int hc12_at_exec(const char *cmd, size_t cmd_len,
		 char *resp, size_t resp_sz,
		 uint32_t quiet_ms,
		 size_t *resp_out_len);

#endif /* CONFIG_AURORA_TELEMETRY_HC12_AT */

#endif /* AURORA_LIB_TELEMETRY_HC12_INTERNAL_H_ */
