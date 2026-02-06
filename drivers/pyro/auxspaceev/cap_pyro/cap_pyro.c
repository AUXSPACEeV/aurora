/*
 * Copyright (c) 2019 Thomas Schmid <tom@lfence.de>
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT auxspaceev_cap_pyro

#include <stdint.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/sensor.h>

#include <aurora/drivers/pyro.h>
#include "cap_pyro.h"

#define LOG_LEVEL CONFIG_PYRO_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cap_pyro);

static int auxspaceev_cap_pyro_init(const struct device *dev)
{
	(void) dev;

	LOG_WRN("auxspaceev_cap_pyro_init unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_cap_pyro_arm_channel(const struct device *dev,
				uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_arm_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_cap_pyro_disarm_channel(const struct device *dev,
					uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_disarm_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_cap_pyro_trigger_channel(const struct device *dev,
					uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_trigger_channel unimplemented.\n");

	return -ENOTSUP;
}


static int auxspaceev_cap_pyro_charge_channel(const struct device *dev,
					uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_charge_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_cap_pyro_read_channel(const struct device *dev,
					uint32_t channel, uint32_t *val)
{
	(void) dev;
	(void) channel;
	(void) val;

	LOG_WRN("auxspaceev_cap_pyro_read_channel unimplemented.\n");

	return -ENOTSUP;
}

static int auxspaceev_cap_pyro_get_nchannels(const struct device *dev)
{
	(void) dev;

	LOG_WRN("auxspaceev_cap_pyro_get_nchannels unimplemented.\n");

	return -ENOTSUP;
}

static DEVICE_API(pyro, pyro_api_funcs) = {
	.arm = auxspaceev_cap_pyro_arm_channel,
	.disarm = auxspaceev_cap_pyro_disarm_channel,
	.trigger = auxspaceev_cap_pyro_trigger_channel,
	.charge = auxspaceev_cap_pyro_charge_channel,
	.read = auxspaceev_cap_pyro_read_channel,
	.get_nchannels = auxspaceev_cap_pyro_get_nchannels,
};

/* Initializes a struct pyro_config for an instance using GPIOs */
#define PYRO_CONFIG(inst)							\
	{									\
		.n_channels = DT_PROP_LEN(DT_DRV_INST(inst), trigger_gpios),	\
		.single_arm = DT_PROP_LEN(DT_DRV_INST(inst), arm_gpios) == 1,	\
		.trigger_gpios = GPIO_DT_SPEC_INST_GET(inst, trigger_gpios),	\
		.arm_gpios = GPIO_DT_SPEC_INST_GET(inst, arm_gpios),		\
		.charge_gpios = GPIO_DT_SPEC_INST_GET(inst, charge_gpios),	\
		.short_gpios = GPIO_DT_SPEC_INST_GET(inst, short_gpios),	\
		.adcs = ADC_DT_SPEC_INST_GET(DT_DRV_INST(inst))			\
	}

/*
* Main instantiation macro, which selects the correct bus-specific
* instantiation macros for the instance.
*/
#define CAP_PYRO_DEFINE(inst)						\
	static struct pyro_data pyro_data_##inst;			\
	static const struct pyro_config pyro_config_##inst =		\
		PYRO_CONFIG(inst);					\
	PYRO_DEVICE_DT_INST_DEFINE(inst,				\
				auxspaceev_cap_pyro_init,		\
				NULL,				\
				&pyro_data_##inst,			\
				&pyro_config_##inst,			\
				POST_KERNEL,				\
				CONFIG_PYRO_INIT_PRIORITY,		\
				&pyro_api_funcs)

/* Create the struct device for every status "okay" node in the devicetree. */
DT_INST_FOREACH_STATUS_OKAY(CAP_PYRO_DEFINE)
 