/**
 * @file mini_pyro.h
 * @brief Minimal pyro driver internal data structures.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AURORA_DRIVERS_MINI_PYRO_H__
#define __AURORA_DRIVERS_MINI_PYRO_H__

#include <zephyr/drivers/gpio.h>
#include <stdint.h>

/**
 * @brief Configuration for the mini pyro driver.
 *
 * Populated from devicetree at compile time via the MINI_PYRO_DEFINE macro.
 * Holds GPIO specs and channel count for a single pyro instance.
 */
struct pyro_config {
	const uint32_t n_channels;		/**< Number of pyro channels. */
	const uint32_t single_arm;		/**< True if a single GPIO arms all channels. */
	const struct gpio_dt_spec *trigger_gpios;/**< Per-channel trigger GPIO specs. */
	const struct gpio_dt_spec *arm_gpios;	/**< Arm GPIO specs (1 or n_channels). */
};

/**
 * @brief Runtime data for the mini pyro driver.
 *
 * Tracks per-channel arm and trigger state as bitmasks.
 */
struct pyro_data {
	uint32_t arm_flags;		/**< Bitmask of armed channels. */
	uint32_t trigger_flags;		/**< Bitmask of triggered channels. */
};

#endif /* __AURORA_DRIVERS_MINI_PYRO_H__*/
