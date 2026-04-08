/**
 * @file basic_pyro.h
 * @brief Basic pyro driver internal data structures.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AURORA_DRIVERS_BASIC_PYRO_H__
#define __AURORA_DRIVERS_BASIC_PYRO_H__

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>

/**
 * @brief Configuration for the basic pyro driver.
 *
 * Populated from devicetree at compile time via the BASIC_PYRO_DEFINE macro.
 * Holds GPIO specs, optional ADC specs, and channel count for a single
 * pyro instance.
 */
struct pyro_config {
	const uint32_t n_channels;		/**< Number of pyro channels. */
	const uint32_t single_arm;		/**< True if a single GPIO arms all channels. */
	const struct gpio_dt_spec *trigger_gpios;/**< Per-channel trigger (fire) GPIO specs. */
	const struct gpio_dt_spec *arm_gpios;	/**< Arm GPIO specs (1 or n_channels). */
	const struct gpio_dt_spec *short_gpios;	/**< Per-channel short/safe GPIO specs, or NULL. */
	const struct gpio_dt_spec *cap_gpios;	/**< Per-channel cap charge GPIO specs, or NULL. */
	const struct adc_dt_spec *capv;		/**< Per-channel cap voltage ADC specs, or NULL. */
	const struct adc_dt_spec *senses;	/**< Per-channel sense ADC specs, or NULL. */
	const uint32_t *capv_max;		/**< Per-channel cap voltage max values, or NULL. */
	const uint32_t *sense_max;		/**< Per-channel sense ADC max values, or NULL. */
};

/**
 * @brief Runtime data for the basic pyro driver.
 *
 * Tracks per-channel arm, trigger, and short state as bitmasks.
 */
struct pyro_data {
	uint32_t arm_flags;		/**< Bitmask of armed channels. */
	uint32_t trigger_flags;		/**< Bitmask of triggered (fired) channels. */
	uint32_t short_flags;		/**< Bitmask of shorted (safe) channels. */
	uint32_t charge_flags;		/**< Bitmask of charging channels. */
};

#endif /* __AURORA_DRIVERS_BASIC_PYRO_H__*/
