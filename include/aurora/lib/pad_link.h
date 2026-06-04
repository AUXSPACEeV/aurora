/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_PAD_LINK_H_
#define AURORA_LIB_PAD_LINK_H_

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
