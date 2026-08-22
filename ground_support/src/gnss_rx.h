/**
 * @file gnss_rx.h
 * @brief GNSS fix receiver for the ground station.
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GROUND_SUPPORT_GNSS_RX_H_
#define GROUND_SUPPORT_GNSS_RX_H_

/**
 * @brief Check the GNSS device is ready.
 *
 * The fix callback itself is registered at link time; this only reports
 * readiness so a missing/unconfigured receiver is visible in the log.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the GNSS device is not ready.
 */
int gnss_rx_init(void);

#endif /* GROUND_SUPPORT_GNSS_RX_H_ */
