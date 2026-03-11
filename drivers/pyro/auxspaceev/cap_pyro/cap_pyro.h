/**
 * @file cap_pyro.h
 * @brief Capacitor-based pyro driver internal data structures.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AURORA_DRIVERS_CAP_PYRO_H__
#define __AURORA_DRIVERS_CAP_PYRO_H__

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>

/**
 * @brief Configuration for the capacitor-based pyro driver.
 *
 * Populated from devicetree at compile time via the CAP_PYRO_DEFINE macro.
 * Holds GPIO specs, ADC specs, and channel count for a single pyro instance.
 */
struct pyro_config {
	const uint32_t n_channels;		/**< Number of pyro channels. */
	const uint32_t single_arm;		/**< True if a single GPIO arms all channels. */
	const uint32_t *sense_max;		/**< Per-channel sense ADC max values, or NULL. */
	const uint32_t *capv_max;		/**< Per-channel capacitor voltage max values, or NULL. */
	struct gpio_dt_spec trigger_gpios;	/**< Trigger GPIO spec. */
	struct gpio_dt_spec arm_gpios;		/**< Arm GPIO spec. */
	struct gpio_dt_spec short_gpios;	/**< Short/safe GPIO spec. */
	struct gpio_dt_spec charge_gpios;	/**< Charge GPIO spec. */
	const struct adc_dt_spec *senses;	/**< Sense ADC specs array, or NULL. */
	const struct adc_dt_spec *capv;		/**< Capacitor voltage ADC specs array, or NULL. */
};

/**
 * @brief Runtime data for the capacitor-based pyro driver.
 *
 * Tracks per-channel arm, trigger, short, and charge state as bitmasks.
 */
struct pyro_data {
	uint32_t arm_flags;		/**< Bitmask of armed channels. */
	uint32_t trigger_flags;		/**< Bitmask of triggered channels. */
	uint32_t short_flags;		/**< Bitmask of shorted/safe channels. */
	uint32_t charge_flags;		/**< Bitmask of charging channels. */
};

#endif /* __AURORA_DRIVERS_CAP_PYRO_H__*/
 