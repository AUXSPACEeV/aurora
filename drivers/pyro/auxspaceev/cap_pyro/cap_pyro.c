/**
 * @file cap_pyro.c
 * @brief Capacitor-based pyro driver implementation.
 *
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

/**
 * @brief Initialize the capacitor-based pyro driver instance.
 *
 * Configures ADC channels for capacitor voltage sensing (if present).
 *
 * @param dev Pyro device instance.
 * @return 0 on success, negative errno on failure.
 */
static int auxspaceev_cap_pyro_init(const struct device *dev)
{
	const struct pyro_config *config = dev->config;
	int ret;

	if (config->capv != NULL) {
		for (uint32_t i = 0; i < config->n_channels; i++) {
			struct adc_channel_cfg cfg = {
				.gain = ADC_GAIN_1_4,
				.reference = ADC_REF_INTERNAL,
				.acquisition_time = ADC_ACQ_TIME_DEFAULT,
				.channel_id = config->capv[i].channel_id,
			};

			ret = adc_channel_setup(config->capv[i].dev, &cfg);
			if (ret < 0) {
				return ret;
			}
		}
	}

	return 0;
}

/** @brief Arm a pyro channel. @note Unimplemented. */
static int auxspaceev_cap_pyro_arm_channel(const struct device *dev,
				uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_arm_channel unimplemented.\n");

	return -ENOTSUP;
}

/** @brief Disarm a pyro channel. @note Unimplemented. */
static int auxspaceev_cap_pyro_disarm_channel(const struct device *dev,
					uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_disarm_channel unimplemented.\n");

	return -ENOTSUP;
}

/** @brief Secure a pyro channel. @note Unimplemented. */
static int auxspaceev_cap_pyro_secure_channel(const struct device *dev,
					      uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_secure_channel unimplemented.\n");

	return -ENOTSUP;
}

/** @brief Trigger a pyro channel. @note Unimplemented. */
static int auxspaceev_cap_pyro_trigger_channel(const struct device *dev,
					uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_trigger_channel unimplemented.\n");

	return -ENOTSUP;
}

/** @brief Charge a pyro channel's capacitor. @note Unimplemented. */
static int auxspaceev_cap_pyro_charge_channel(const struct device *dev,
					uint32_t channel)
{
	(void) dev;
	(void) channel;

	LOG_WRN("auxspaceev_cap_pyro_charge_channel unimplemented.\n");

	return -ENOTSUP;
}

/** @brief Read a pyro channel's continuity sense value. @note Unimplemented. */
static int auxspaceev_cap_pyro_sense_channel(const struct device *dev,
					     uint32_t channel, uint32_t *val)
{
	(void) dev;
	(void) channel;
	(void) val;

	LOG_WRN("auxspaceev_cap_pyro_sense_channel unimplemented.\n");

	return -ENOTSUP;
}

/** @brief Read a pyro channel's capacitor voltage. @note Unimplemented. */
static int auxspaceev_cap_pyro_read_cap_channel(const struct device *dev,
						uint32_t channel, uint32_t *val)
{
	(void) dev;
	(void) channel;
	(void) val;

	LOG_WRN("auxspaceev_cap_pyro_read_cap_channel unimplemented.\n");

	return -ENOTSUP;
}

/** @brief Get the number of pyro channels. @note Unimplemented. */
static int auxspaceev_cap_pyro_get_nchannels(const struct device *dev)
{
	(void) dev;

	LOG_WRN("auxspaceev_cap_pyro_get_nchannels unimplemented.\n");

	return -ENOTSUP;
}

static DEVICE_API(pyro, pyro_api_funcs) = {
	.arm = auxspaceev_cap_pyro_arm_channel,
	.disarm = auxspaceev_cap_pyro_disarm_channel,
	.secure = auxspaceev_cap_pyro_secure_channel,
	.trigger = auxspaceev_cap_pyro_trigger_channel,
	.charge = auxspaceev_cap_pyro_charge_channel,
	.sense = auxspaceev_cap_pyro_sense_channel,
	.read_cap = auxspaceev_cap_pyro_read_cap_channel,
	.get_nchannels = auxspaceev_cap_pyro_get_nchannels,
};

/**
 * @brief Build an adc_dt_spec from a phandle-array property element.
 *
 * @param node_id Devicetree node identifier.
 * @param prop    Phandle-array property name (e.g. capv_adcs).
 * @param idx     Element index within the phandle-array.
 */
#define CAP_PYRO_ADC_SPEC(node_id, prop, idx)				\
	ADC_DT_SPEC_STRUCT(DT_PHANDLE_BY_IDX(node_id, prop, idx),	\
			   DT_PHA_BY_IDX(node_id, prop, idx, input))

/**
 * @brief Declare a static adc_dt_spec array for a phandle-array property.
 *
 * @param inst Instance number.
 * @param prop Phandle-array property name.
 */
#define CAP_PYRO_ADC_ARRAY(inst, prop)						\
	static const struct adc_dt_spec pyro_##prop##_##inst[] = {		\
		DT_FOREACH_PROP_ELEM_SEP(DT_DRV_INST(inst), prop,		\
					 CAP_PYRO_ADC_SPEC, (,))		\
	}

/**
 * @brief Declare a static uint32_t array from a devicetree array property.
 *
 * @param inst Instance number.
 * @param prop Array property name (e.g. capv_max, sense_max).
 */
#define CAP_PYRO_U32_ARRAY(inst, prop)						\
	static const uint32_t pyro_##prop##_##inst[] =				\
		DT_PROP(DT_DRV_INST(inst), prop)

/**
 * @brief Initialize a struct pyro_config from devicetree GPIO/ADC bindings.
 *
 * @param inst Instance number.
 */
#define PYRO_CONFIG(inst)								\
	{										\
		.n_channels = DT_PROP_LEN(DT_DRV_INST(inst), trigger_gpios),		\
		.single_arm = DT_PROP_LEN(DT_DRV_INST(inst), arm_gpios) == 1,		\
		.sense_max = COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), sense_max),\
				(pyro_sense_max_##inst), (NULL)),			\
		.capv_max = COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), capv_max),	\
				(pyro_capv_max_##inst), (NULL)),			\
		.trigger_gpios = GPIO_DT_SPEC_INST_GET(inst, trigger_gpios),		\
		.arm_gpios = GPIO_DT_SPEC_INST_GET(inst, arm_gpios),			\
		.charge_gpios = GPIO_DT_SPEC_INST_GET(inst, charge_gpios),		\
		.short_gpios = GPIO_DT_SPEC_INST_GET(inst, short_gpios),		\
		.senses = COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), sense_adcs),	\
				(pyro_sense_adcs_##inst), (NULL)),			\
		.capv = COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), capv_adcs),	\
				(pyro_capv_adcs_##inst), (NULL)),			\
	}

/**
 * @brief Instantiate a capacitor-based pyro driver from devicetree.
 *
 * Declares optional ADC and max-value arrays, allocates runtime data,
 * builds the configuration struct, and registers the device.
 *
 * @param inst Instance number (from DT_INST_FOREACH_STATUS_OKAY).
 */
#define CAP_PYRO_DEFINE(inst)						\
	IF_ENABLED(DT_NODE_HAS_PROP(DT_DRV_INST(inst), capv_adcs),	\
		   (CAP_PYRO_ADC_ARRAY(inst, capv_adcs);))		\
	IF_ENABLED(DT_NODE_HAS_PROP(DT_DRV_INST(inst), sense_adcs),	\
		   (CAP_PYRO_ADC_ARRAY(inst, sense_adcs);))		\
	IF_ENABLED(DT_NODE_HAS_PROP(DT_DRV_INST(inst), capv_max),	\
		   (CAP_PYRO_U32_ARRAY(inst, capv_max);))		\
	IF_ENABLED(DT_NODE_HAS_PROP(DT_DRV_INST(inst), sense_max),	\
		   (CAP_PYRO_U32_ARRAY(inst, sense_max);))		\
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
 