/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_TELEMETRY_WIRE_H_
#define AURORA_LIB_TELEMETRY_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/toolchain.h>

/**
 * @defgroup lib_telemetry_wire Telemetry wire frame
 * @ingroup lib_telemetry
 * @{
 *
 * @brief The on-the-wire format shared by every telemetry transport.
 *
 * This header is the single definition of the format, because senders
 * and receivers are separate firmwares: the UART link backends in
 * lib/telemetry write these frames, and the AUX-Tel relay (the @c abby
 * application) parses them. A mismatch is only visible as silent
 * packet loss, so there is one definition and every side includes it.
 *
 * It is deliberately free of Kconfig and library dependencies - a
 * receiver that only relays frames needs the framing, not the
 * dispatcher.
 *
 * Layout (all multi-byte fields little-endian):
 *
 *     0        magic0 = 0xA5
 *     1        magic1 = 0x5A
 *     2        type
 *     3        len         payload bytes that follow, excluding the CRC
 *     4..4+len payload
 *     4+len    CRC-16/CCITT (init 0xFFFF) over bytes [2 .. 4+len-1],
 *              i.e. over type, len and payload
 *
 * The frame is self-describing on purpose: @c type plus the @c sm_type
 * field inside an SM_UPDATE payload let a receiver decode a frame
 * without prior agreement with the sending firmware, and the magic
 * pair lets it resynchronise after corruption.
 */

/** @brief First sync byte. */
#define AURORA_TELEMETRY_WIRE_MAGIC0 0xA5
/** @brief Second sync byte. */
#define AURORA_TELEMETRY_WIRE_MAGIC1 0x5A

/** @brief Header length: magic0, magic1, type, len. */
#define AURORA_TELEMETRY_WIRE_HDR_LEN 4
/** @brief Trailing CRC length. */
#define AURORA_TELEMETRY_WIRE_CRC_LEN 2

/** @brief Largest payload the @c len byte can describe. */
#define AURORA_TELEMETRY_WIRE_MAX_PAYLOAD 255

/** @brief Frame overhead: header plus CRC. */
#define AURORA_TELEMETRY_WIRE_OVERHEAD			\
	(AURORA_TELEMETRY_WIRE_HDR_LEN + AURORA_TELEMETRY_WIRE_CRC_LEN)

/** @brief Largest frame the format can produce. */
#define AURORA_TELEMETRY_WIRE_MAX_FRAME			\
	(AURORA_TELEMETRY_WIRE_OVERHEAD + AURORA_TELEMETRY_WIRE_MAX_PAYLOAD)

/**
 * @brief Packet types.
 *
 * Relays are type-agnostic: a new type travels end to end without
 * anything in between having to learn it.
 */
#define AURORA_TELEMETRY_WIRE_TYPE_SM_UPDATE 0x01
/** @brief Low-rate "still here" heartbeat; see @ref telemetry_wire_status. */
#define AURORA_TELEMETRY_WIRE_TYPE_STATUS    0x02

/**
 * @brief SM_UPDATE payload (64 bytes, packed, little-endian).
 *
 * @c sm_type identifies the @c sm_state enum mapping in use (see
 * @ref sm_get_type) so a receiver can decode @c state without prior
 * agreement with the firmware that sent it. The trailing @c reserved
 * byte keeps the 64-byte size unchanged from earlier revisions.
 *
 * Changing this layout is a wire break: every receiver, including
 * ground-station tooling, decodes it by offset.
 */
struct __packed telemetry_wire_sm_update {
	uint32_t timestamp_ms;
	uint8_t  state;
	uint8_t  armed;
	uint8_t  sm_type;
	uint8_t  reserved;
	double   altitude;
	double   acceleration;
	double   accel_vert;
	double   velocity;
	double   orientation[3];
};

/** @name STATUS flag bits
 *  Bits of @ref telemetry_wire_status.flags. Unassigned bits are zero.
 *  @{
 */
/** @brief The arm input is asserted. */
#define AURORA_TELEMETRY_WIRE_STATUS_ARMED      (1U << 0)
/** @brief The IMU initialised and delivered a sample recently. */
#define AURORA_TELEMETRY_WIRE_STATUS_IMU_OK     (1U << 1)
/** @brief The barometer initialised and delivered a sample recently. */
#define AURORA_TELEMETRY_WIRE_STATUS_BARO_OK    (1U << 2)
/** @brief Attitude calibration has converged. */
#define AURORA_TELEMETRY_WIRE_STATUS_CALIBRATED (1U << 3)
/** @brief The flight log is online, so arming will not be refused for it. */
#define AURORA_TELEMETRY_WIRE_STATUS_LOG_READY  (1U << 4)
/** @} */

/**
 * @brief STATUS payload (8 bytes, packed, little-endian).
 *
 * A heartbeat that does not depend on the sensors: it goes out while
 * the vehicle is not in flight even when no SM_UPDATE can be produced,
 * so a ground station can tell "board up, IMU dead" from "board off".
 * Kept small because on a duty-cycled radio its airtime competes with
 * SM_UPDATE frames.
 *
 * Changing this layout is a wire break, as for SM_UPDATE.
 */
struct __packed telemetry_wire_status {
	uint32_t timestamp_ms;
	uint8_t  state;
	uint8_t  sm_type;
	uint8_t  flags;
	uint8_t  reserved;
};

/**
 * @brief CRC over a frame's covered span.
 *
 * @param frame        Frame buffer, from @c magic0.
 * @param payload_len  Value of the frame's @c len byte.
 *
 * @return CRC-16/CCITT over @c type, @c len and the payload.
 */
static inline uint16_t telemetry_wire_crc(const uint8_t *frame,
					  uint8_t payload_len)
{
	return crc16_ccitt(0xFFFF, &frame[2], (size_t)payload_len + 2);
}

/**
 * @brief Build a complete frame in @p buf.
 *
 * @param buf          Output buffer.
 * @param buf_sz       Size of @p buf in bytes.
 * @param type         Packet type byte.
 * @param payload      Payload bytes.
 * @param payload_len  Length of @p payload.
 *
 * @return Total frame length written, or 0 if @p buf is too small.
 */
static inline size_t telemetry_wire_finalise(uint8_t *buf, size_t buf_sz,
					     uint8_t type,
					     const void *payload,
					     uint8_t payload_len)
{
	const size_t total = (size_t)AURORA_TELEMETRY_WIRE_OVERHEAD +
			     payload_len;

	if (!buf || buf_sz < total) {
		return 0;
	}

	buf[0] = AURORA_TELEMETRY_WIRE_MAGIC0;
	buf[1] = AURORA_TELEMETRY_WIRE_MAGIC1;
	buf[2] = type;
	buf[3] = payload_len;

	if (payload_len) {
		memcpy(&buf[AURORA_TELEMETRY_WIRE_HDR_LEN], payload,
		       payload_len);
	}

	sys_put_le16(telemetry_wire_crc(buf, payload_len),
		     &buf[AURORA_TELEMETRY_WIRE_HDR_LEN + payload_len]);

	return total;
}

/**
 * @brief Validate a whole frame that arrived as one packet.
 *
 * For transports with packet boundaries - a LoRa packet, a datagram -
 * where the entire frame is delivered at once and no byte-stream
 * resynchronisation is involved. Checks the magic pair, that the frame's
 * own @c len byte agrees with the number of bytes actually received, and
 * the CRC.
 *
 * The length cross-check is what makes this safe to run on a buffer
 * straight off a radio: a corrupt @c len byte would otherwise send
 * @ref telemetry_wire_crc reading past what was received.
 *
 * @param frame  Frame buffer, from @c magic0.
 * @param len    Number of bytes received.
 *
 * @retval true if @p frame is a well-formed frame of exactly @p len bytes.
 */
static inline bool telemetry_wire_validate(const uint8_t *frame, size_t len)
{
	if (!frame || len < (size_t)AURORA_TELEMETRY_WIRE_OVERHEAD) {
		return false;
	}

	if (frame[0] != AURORA_TELEMETRY_WIRE_MAGIC0 ||
	    frame[1] != AURORA_TELEMETRY_WIRE_MAGIC1) {
		return false;
	}

	const uint8_t payload_len = frame[3];

	if (len != (size_t)AURORA_TELEMETRY_WIRE_OVERHEAD + payload_len) {
		return false;
	}

	return telemetry_wire_crc(frame, payload_len) ==
	       sys_get_le16(&frame[AURORA_TELEMETRY_WIRE_HDR_LEN + payload_len]);
}

/** @} */

#endif /* AURORA_LIB_TELEMETRY_WIRE_H_ */
