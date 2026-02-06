/*
 * Copyright (c) 2019 Thomas Schmid <tom@lfence.de>
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT auxspaceev_mini_pyro

#include <stdint.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/sensor.h>

#include <aurora/drivers/pyro.h>
#include "mini_pyro.h"

#define LOG_LEVEL CONFIG_PYRO_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mini_pyro);

static int auxspaceev_mini_pyro_init(const struct device *dev)
{
	(void) dev;

	LOG_WRN("auxspaceev_mini_pyro_init unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_mini_pyro_arm_channel(const struct device *dev,
				       uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_mini_pyro_arm_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_mini_pyro_disarm_channel(const struct device *dev,
					  uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_mini_pyro_disarm_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_mini_pyro_trigger_channel(const struct device *dev,
					   uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_mini_pyro_trigger_channel unimplemented.\n");

	return -ENOTSUP;
}


static int auxspaceev_mini_pyro_charge_channel(const struct device *dev,
					       uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_mini_pyro_charge_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_mini_pyro_read_channel(const struct device *dev,
					uint32_t channel, uint32_t *val)
{
	(void) dev;
	(void) channel;
	(void) val;

	LOG_WRN("auxspaceev_mini_pyro_read_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_mini_pyro_get_nchannels(const struct device *dev)
{
	(void) dev;

	LOG_WRN("auxspaceev_mini_pyro_get_nchannels unimplemented.\n");

	return -ENOTSUP;
}

static DEVICE_API(pyro, pyro_api_funcs) = {
	.arm = auxspaceev_mini_pyro_arm_channel,
	.disarm = auxspaceev_mini_pyro_disarm_channel,
	.trigger = auxspaceev_mini_pyro_trigger_channel,
	.charge = auxspaceev_mini_pyro_charge_channel,
	.read = auxspaceev_mini_pyro_read_channel,
	.get_nchannels = auxspaceev_mini_pyro_get_nchannels,
};

/* Initializes a struct pyro_config for an instance using GPIOs */
#define PYRO_CONFIG(inst)							\
	{									\
		.n_channels = DT_PROP_LEN(DT_DRV_INST(inst), trigger_gpios),	\
		.single_arm = DT_PROP_LEN(DT_DRV_INST(inst), arm_gpios) == 1,	\
		.trigger_gpios = GPIO_DT_SPEC_INST_GET(inst, trigger_gpios),	\
		.arm_gpios = GPIO_DT_SPEC_INST_GET(inst, arm_gpios),		\
	}

/*
* Main instantiation macro, which selects the correct bus-specific
* instantiation macros for the instance.
*/
#define MINI_PYRO_DEFINE(inst)						\
	static struct pyro_data pyro_data_##inst;			\
	static const struct pyro_config pyro_config_##inst =		\
		PYRO_CONFIG(inst);					\
	PYRO_DEVICE_DT_INST_DEFINE(inst,				\
				   auxspaceev_mini_pyro_init,		\
				   NULL,				\
				   &pyro_data_##inst,			\
				   &pyro_config_##inst,			\
				   POST_KERNEL,				\
				   CONFIG_PYRO_INIT_PRIORITY,		\
				   &pyro_api_funcs)

/* Create the struct device for every status "okay" node in the devicetree. */
DT_INST_FOREACH_STATUS_OKAY(MINI_PYRO_DEFINE)
