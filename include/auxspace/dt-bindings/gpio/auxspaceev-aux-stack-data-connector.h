/*
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic pin names for the AUX data-stack connector's "information" GPIO
 * group (the SIGNALS columns in AUX-Core's stack_data.drawio.svg pinmap).
 * CAN/I2C/UART are not listed here - they're plain bus phandles/aliases,
 * not part of the gpio-map (see the connector binding for why).
 *
 * These macro values are just gpio-map indices; they carry no meaning
 * outside of this connector's own gpio-map entries. Every brain board uses
 * the same macros here, pointing at whatever physical GPIO that board
 * actually wires them to.
 */
#ifndef AURORA_INCLUDE_DT_BINDINGS_GPIO_AUX_STACK_DATA_CONNECTOR_H_
#define AURORA_INCLUDE_DT_BINDINGS_GPIO_AUX_STACK_DATA_CONNECTOR_H_

#define AUX_DATA_ARM     0
#define AUX_DATA_LIFTOFF 1
#define AUX_DATA_PWRFLT  2
#define AUX_DATA_BAT_ON  3

#endif /* AURORA_INCLUDE_DT_BINDINGS_GPIO_AUX_DATA_CONNECTOR_H_ */
