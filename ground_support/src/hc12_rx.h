/**
 * @file hc12_rx.h
 * @brief HC-12 telemetry receiver for the ground station.
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GROUND_SUPPORT_HC12_RX_H_
#define GROUND_SUPPORT_HC12_RX_H_

/**
 * @brief Start receiving rocket telemetry on the HC-12 UART.
 *
 * Enables interrupt-driven RX on the UART behind the @c hc12 devicetree node;
 * a worker thread reassembles frames and publishes decoded telemetry into the
 * shared snapshot.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the UART device is not ready.
 */
int hc12_rx_init(void);

#endif /* GROUND_SUPPORT_HC12_RX_H_ */
