/*
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AURORA_DRIVERS_CAP_PYRO_H__
#define __AURORA_DRIVERS_CAP_PYRO_H__
 
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>

struct pyro_config {
	const uint32_t n_channels;
	const uint32_t single_arm;
	const uint32_t *sense_max;
	const uint32_t *capv_max;
	struct gpio_dt_spec trigger_gpios;
	struct gpio_dt_spec arm_gpios;
	struct gpio_dt_spec short_gpios;
	struct gpio_dt_spec charge_gpios;
	const struct adc_dt_spec *senses;
	const struct adc_dt_spec *capv;
};

struct pyro_data {
	uint32_t arm_flags;
	uint32_t trigger_flags;
	uint32_t short_flags;
	uint32_t charge_flags;
};

#endif /* __AURORA_DRIVERS_CAP_PYRO_H__*/
 