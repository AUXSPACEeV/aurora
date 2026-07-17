/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_PAD_LINK_H_
#define AURORA_LIB_PAD_LINK_H_

#include <stdint.h>

#include <aurora/lib/state/state.h>

/**
 * @defgroup lib_pad_link Pad-link BLE status server
 * @ingroup lib
 * @{
 *
 * @brief BLE GATT peripheral that exposes flight-state, raw sensor values,
 *        and computed kinematics to a pad-side central
 *        (the launchrail or ground station).
 *
 * Intended for use while the rocket is on the pad: passive status
 * surface, not a flight downlink. Nothing on the rocket waits on the
 * link, so loss of connection (inevitable at launch) is a non-event:
 * the disconnect callback simply restarts advertising.
 */

/* Capability register (uint32, little-endian), exposed on characteristic a0.
 *
 * The application composes this value from the PL_CAP_* flags below and
 * hands it to pad_link_set_caps(); the library only stores and serves it.
 *
 * Byte 0 — IMU group
 *   [2:0]  IMU type    enum pl_cap_imu_type
 *   [3]    Accel       1 = accelerometer data valid (a2)
 *   [4]    Gyro        1 = gyrometer data valid (a3)
 *   [7:5]  reserved
 *
 * Byte 1 — Environmental group
 *   [8]    Baro        1 = baro data valid (a1)
 *   [9]    Inner temp  1 = inner_temp data valid (a7)
 *   [10]   Motor temp  1 = motor_temp data valid (a8, planned)
 *   [11]   Hull temp   1 = hull_temp data valid  (a9, planned)
 *   [15:12] reserved
 *
 * Byte 2 — Positioning group
 *   [16]   GPS/GNSS    1 = gps data valid (a6, planned)
 *   [23:17] reserved
 *
 * Byte 3 — reserved
 */
enum pl_cap_imu_type {
	PL_CAP_IMU_TYPE_NONE = 0x0,
	PL_CAP_IMU_TYPE_6DOF = 0x1,
	PL_CAP_IMU_TYPE_9DOF = 0x2,
};
#define PL_CAP_IMU_TYPE_MASK  (0x7u << 0)
#define PL_CAP_IMU_TYPE(t)    (((uint32_t)(t)) << 0)
#define PL_CAP_ACCEL          (1u << 3)
#define PL_CAP_GYRO           (1u << 4)

#define PL_CAP_BARO           (1u << 8)
#define PL_CAP_TEMP_INNER     (1u << 9)
#define PL_CAP_TEMP_MOTOR     (1u << 10)
#define PL_CAP_TEMP_HULL      (1u << 11)

#define PL_CAP_GPS            (1u << 16)

/**
 * @brief Bring up the BLE stack and start advertising.
 *
 * Idempotent within a boot: returns the @c bt_enable error on failure
 * and does not retry. The caller should treat a failure as
 * "no pad link this boot" and continue.
 *
 * @retval 0 on success.
 * @retval <0 propagated from @c bt_enable.
 */
int pad_link_init(void);

/**
 * @brief Declare which sensor characteristics carry valid data.
 *
 * Publishes @p caps on the boardcap characteristic (a0). What the board
 * actually carries is hardware knowledge the application has and the
 * library does not, so the application composes the value from the
 * @c PL_CAP_* flags and calls this once during init (any time before a
 * central connects). Until then the register reads as 0 — "no
 * capabilities". Safe to call from any context; never blocks.
 *
 * @param caps  Capability register value, see @c PL_CAP_*.
 */
void pad_link_set_caps(uint32_t caps);

/**
 * @brief Publish the latest SM state and computed kinematics.
 *
 * Updates the read snapshot and, for any subscribed central, fires a
 * GATT notification. Safe to call from the SM thread; never blocks.
 *
 * @c type lets the central pick the right @c sm_state enum mapping
 *
 * @param state   Current flight state.
 * @param type    Active state machine implementation ID
 *                (see @ref sm_get_type). Constant across a boot.
 * @param inputs  Snapshot returned by @c sm_get_inputs.
 */
void pad_link_publish_sm(enum sm_state state, enum sm_type type,
			 const struct sm_inputs *inputs);

/** @} */

#endif /* AURORA_LIB_PAD_LINK_H_ */
