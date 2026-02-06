/*
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AURORA_DRIVERS_MINI_PYRO_H__
#define __AURORA_DRIVERS_MINI_PYRO_H__

#include <zephyr/drivers/gpio.h>
#include <stdint.h>

/* Forward declaration */
struct pyro_config;
struct pyro_data;

struct pyro_config {
	const uint32_t n_channels;
	const uint32_t single_arm;
	struct gpio_dt_spec trigger_gpios;
	struct gpio_dt_spec arm_gpios;
};

struct pyro_data {
	uint32_t arm_flags;
	uint32_t trigger_flags;
};

#endif /* __AURORA_DRIVERS_MINI_PYRO_H__*/
