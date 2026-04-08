/**
 * @file mini_pyro.c
 * @brief Minimal capacitor-based pyro driver implementation.
 *
 * Hardware model (per channel):
 *   - Arm GPIO   : charges the pyro capacitor.
 *   - Short GPIO : shorts across the pyro to make it safe.
 *   - Trigger GPIO: connects charged capacitor to the pyro (fire).
 *   - Cap-V ADC  : measures capacitor voltage (0-9 V range).
 *   - Sense ADC  : measures pyro resistance for health check.
 *
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

/**
 * @brief Initialize the mini pyro driver instance.
 *
 * Configures trigger, arm, and (optional) short GPIOs as output-inactive.
 * Sets up ADC channels for cap-voltage and sense readings if present.
 *
 * @param dev Pyro device instance.
 * @return 0 on success, negative errno on failure.
 */
static int auxspaceev_mini_pyro_init(const struct device *dev)
{
	const struct pyro_config *config = dev->config;
	int ret;

	for (uint32_t i = 0; i < config->n_channels; i++) {
		if (!gpio_is_ready_dt(&config->trigger_gpios[i])) {
			LOG_ERR("trigger gpio %u not ready", i);
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&config->trigger_gpios[i],
					    GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("failed to configure trigger gpio %u: %d",
				i, ret);
			return ret;
		}
	}

	uint32_t n_arm = config->single_arm ? 1 : config->n_channels;

	for (uint32_t i = 0; i < n_arm; i++) {
		if (!gpio_is_ready_dt(&config->arm_gpios[i])) {
			LOG_ERR("arm gpio %u not ready", i);
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&config->arm_gpios[i],
					    GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("failed to configure arm gpio %u: %d",
				i, ret);
			return ret;
		}
	}

	if (config->short_gpios != NULL) {
		for (uint32_t i = 0; i < config->n_channels; i++) {
			if (!gpio_is_ready_dt(&config->short_gpios[i])) {
				LOG_ERR("short gpio %u not ready", i);
				return -ENODEV;
			}
			ret = gpio_pin_configure_dt(&config->short_gpios[i],
						    GPIO_OUTPUT_INACTIVE);
			if (ret < 0) {
				LOG_ERR("failed to configure short gpio %u: %d",
					i, ret);
				return ret;
			}
		}
	}

	if (config->capv != NULL) {
		for (uint32_t i = 0; i < config->n_channels; i++) {
			ret = adc_channel_setup_dt(&config->capv[i]);
			if (ret < 0) {
				LOG_ERR("failed to setup capv adc %u: %d",
					i, ret);
				return ret;
			}
		}
	}

	if (config->senses != NULL) {
		for (uint32_t i = 0; i < config->n_channels; i++) {
			ret = adc_channel_setup_dt(&config->senses[i]);
			if (ret < 0) {
				LOG_ERR("failed to setup sense adc %u: %d",
					i, ret);
				return ret;
			}
		}
	}

	return 0;
}

/**
 * @brief Arm a pyro channel (charges the capacitor).
 *
 * Deasserts the short GPIO first (if present) to allow charging,
 * then asserts the arm GPIO to begin capacitor charging.
 */
static int auxspaceev_mini_pyro_arm_channel(const struct device *dev,
					    uint32_t channel)
{
	const struct pyro_config *config = dev->config;
	struct pyro_data *data = dev->data;
	int ret;

	if (channel >= config->n_channels) {
		return -EINVAL;
	}

	/* Release the short before charging */
	if (config->short_gpios != NULL &&
	    (data->short_flags & BIT(channel))) {
		ret = gpio_pin_set_dt(&config->short_gpios[channel], 0);
		if (ret < 0) {
			return ret;
		}
		data->short_flags &= ~BIT(channel);
	}

	uint32_t arm_idx = config->single_arm ? 0 : channel;

	ret = gpio_pin_set_dt(&config->arm_gpios[arm_idx], 1);
	if (ret < 0) {
		return ret;
	}

	data->arm_flags |= BIT(channel);

	return 0;
}

/** @brief Disarm a pyro channel (stops capacitor charging). */
static int auxspaceev_mini_pyro_disarm_channel(const struct device *dev,
					       uint32_t channel)
{
	const struct pyro_config *config = dev->config;
	struct pyro_data *data = dev->data;

	if (channel >= config->n_channels) {
		return -EINVAL;
	}

	data->arm_flags &= ~BIT(channel);

	/*
	 * In single_arm mode, only deassert the shared arm GPIO
	 * when no channels remain armed.
	 */
	uint32_t arm_idx = config->single_arm ? 0 : channel;

	if (!config->single_arm || data->arm_flags == 0) {
		int ret = gpio_pin_set_dt(&config->arm_gpios[arm_idx], 0);

		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

/**
 * @brief Secure a pyro channel.
 *
 * Deasserts the trigger, asserts the short GPIO (shorts across the
 * pyro to make it safe), and disarms (stops charging).
 */
static int auxspaceev_mini_pyro_secure_channel(const struct device *dev,
					       uint32_t channel)
{
	const struct pyro_config *config = dev->config;
	struct pyro_data *data = dev->data;
	int ret;

	if (channel >= config->n_channels) {
		return -EINVAL;
	}

	/* Deassert trigger first */
	ret = gpio_pin_set_dt(&config->trigger_gpios[channel], 0);
	if (ret < 0) {
		return ret;
	}
	data->trigger_flags &= ~BIT(channel);

	/* Short across the pyro to make it safe */
	if (config->short_gpios != NULL) {
		ret = gpio_pin_set_dt(&config->short_gpios[channel], 1);
		if (ret < 0) {
			return ret;
		}
		data->short_flags |= BIT(channel);
	}

	return auxspaceev_mini_pyro_disarm_channel(dev, channel);
}

/**
 * @brief Trigger (fire) a pyro channel.
 *
 * Connects the charged capacitor to the pyro. Only succeeds if the
 * channel is armed (capacitor charging/charged).
 */
static int auxspaceev_mini_pyro_trigger_channel(const struct device *dev,
						uint32_t channel)
{
	const struct pyro_config *config = dev->config;
	struct pyro_data *data = dev->data;

	if (channel >= config->n_channels) {
		return -EINVAL;
	}

	if (!(data->arm_flags & BIT(channel))) {
		LOG_ERR("channel %u not armed, capacitor not charged", channel);
		return -EACCES;
	}

	int ret = gpio_pin_set_dt(&config->trigger_gpios[channel], 1);

	if (ret < 0) {
		return ret;
	}

	data->trigger_flags |= BIT(channel);

	return 0;
}

/**
 * @brief Charge a pyro channel's capacitor.
 *
 * On mini-pyro hardware, arming and charging are the same operation.
 */
static int auxspaceev_mini_pyro_charge_channel(const struct device *dev,
					       uint32_t channel)
{
	return auxspaceev_mini_pyro_arm_channel(dev, channel);
}

/**
 * @brief Read a pyro channel's continuity sense value (pyro health).
 *
 * Measures pyro resistance via the sense ADC. The returned value is
 * the raw ADC millivolt reading.
 */
static int auxspaceev_mini_pyro_sense_channel(const struct device *dev,
					      uint32_t channel, uint32_t *val)
{
	const struct pyro_config *config = dev->config;
	int16_t buf;
	int ret;

	if (channel >= config->n_channels) {
		return -EINVAL;
	}

	if (config->senses == NULL) {
		return -ENOTSUP;
	}

	struct adc_sequence seq = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};

	adc_sequence_init_dt(&config->senses[channel], &seq);

	ret = adc_read_dt(&config->senses[channel], &seq);
	if (ret < 0) {
		return ret;
	}

	int32_t mv = (int32_t)buf;

	ret = adc_raw_to_millivolts_dt(&config->senses[channel], &mv);
	if (ret < 0) {
		return ret;
	}

	*val = (uint32_t)mv;

	return 0;
}

/**
 * @brief Read a pyro channel's capacitor voltage.
 *
 * Measures the capacitor voltage via the cap-V ADC. The returned
 * value is in millivolts.
 */
static int auxspaceev_mini_pyro_read_cap_channel(const struct device *dev,
						 uint32_t channel,
						 uint32_t *val)
{
	const struct pyro_config *config = dev->config;
	int16_t buf;
	int ret;

	if (channel >= config->n_channels) {
		return -EINVAL;
	}

	if (config->capv == NULL) {
		return -ENOTSUP;
	}

	struct adc_sequence seq = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};

	adc_sequence_init_dt(&config->capv[channel], &seq);

	ret = adc_read_dt(&config->capv[channel], &seq);
	if (ret < 0) {
		return ret;
	}

	int32_t mv = (int32_t)buf;

	ret = adc_raw_to_millivolts_dt(&config->capv[channel], &mv);
	if (ret < 0) {
		return ret;
	}

	*val = (uint32_t)mv;

	return 0;
}

/** @brief Get the number of pyro channels. */
static int auxspaceev_mini_pyro_get_nchannels(const struct device *dev)
{
	const struct pyro_config *config = dev->config;

	return config->n_channels;
}

static DEVICE_API(pyro, pyro_api_funcs) = {
	.arm = auxspaceev_mini_pyro_arm_channel,
	.disarm = auxspaceev_mini_pyro_disarm_channel,
	.secure = auxspaceev_mini_pyro_secure_channel,
	.trigger = auxspaceev_mini_pyro_trigger_channel,
	.charge = auxspaceev_mini_pyro_charge_channel,
	.sense = auxspaceev_mini_pyro_sense_channel,
	.read_cap = auxspaceev_mini_pyro_read_cap_channel,
	.get_nchannels = auxspaceev_mini_pyro_get_nchannels,
};

/**
 * @brief Build a gpio_dt_spec initializer from a phandle-array element.
 *
 * @param node_id Devicetree node identifier.
 * @param prop    Phandle-array property name.
 * @param idx     Element index within the phandle-array.
 */
#define MINI_PYRO_GPIO_SPEC(node_id, prop, idx)				\
	GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

/**
 * @brief Build an adc_dt_spec from a phandle-array io-channel element.
 *
 * @param node_id Devicetree node identifier.
 * @param prop    Phandle-array property name.
 * @param idx     Element index within the phandle-array.
 */
#define MINI_PYRO_ADC_SPEC(node_id, prop, idx)				\
	ADC_DT_SPEC_STRUCT(DT_PHANDLE_BY_IDX(node_id, prop, idx),	\
			   DT_PHA_BY_IDX(node_id, prop, idx, input)),

/**
 * @brief Initialize a struct pyro_config from devicetree bindings.
 *
 * @param inst Instance number.
 */
#define PYRO_CONFIG(inst)							\
	{									\
		.n_channels = DT_PROP_LEN(DT_DRV_INST(inst), trigger_gpios),	\
		.single_arm = DT_PROP_LEN(DT_DRV_INST(inst), arm_gpios) == 1,	\
		.trigger_gpios = pyro_trigger_gpios_##inst,			\
		.arm_gpios = pyro_arm_gpios_##inst,				\
		.short_gpios = COND_CODE_1(					\
			DT_NODE_HAS_PROP(DT_DRV_INST(inst), short_gpios),	\
			(pyro_short_gpios_##inst), (NULL)),			\
		.capv = COND_CODE_1(						\
			DT_NODE_HAS_PROP(DT_DRV_INST(inst), capv_adcs),	\
			(pyro_capv_adcs_##inst), (NULL)),			\
		.senses = COND_CODE_1(						\
			DT_NODE_HAS_PROP(DT_DRV_INST(inst), sense_adcs),	\
			(pyro_sense_adcs_##inst), (NULL)),			\
	}

/**
 * @brief Instantiate a mini pyro driver from devicetree.
 *
 * Declares per-channel GPIO and ADC spec arrays, allocates runtime data,
 * builds the configuration struct, and registers the device.
 *
 * @param inst Instance number (from DT_INST_FOREACH_STATUS_OKAY).
 */
#define MINI_PYRO_DEFINE(inst)						\
	static const struct gpio_dt_spec pyro_trigger_gpios_##inst[] = {\
		DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), trigger_gpios,	\
				     MINI_PYRO_GPIO_SPEC)		\
	};								\
	static const struct gpio_dt_spec pyro_arm_gpios_##inst[] = {	\
		DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), arm_gpios,	\
				     MINI_PYRO_GPIO_SPEC)		\
	};								\
	IF_ENABLED(DT_NODE_HAS_PROP(DT_DRV_INST(inst), short_gpios), (	\
	static const struct gpio_dt_spec pyro_short_gpios_##inst[] = {	\
		DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), short_gpios,	\
				     MINI_PYRO_GPIO_SPEC)		\
	};								\
	))								\
	IF_ENABLED(DT_NODE_HAS_PROP(DT_DRV_INST(inst), capv_adcs), (	\
	static const struct adc_dt_spec pyro_capv_adcs_##inst[] = {	\
		DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), capv_adcs,	\
				     MINI_PYRO_ADC_SPEC)		\
	};								\
	))								\
	IF_ENABLED(DT_NODE_HAS_PROP(DT_DRV_INST(inst), sense_adcs), (	\
	static const struct adc_dt_spec pyro_sense_adcs_##inst[] = {	\
		DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), sense_adcs,	\
				     MINI_PYRO_ADC_SPEC)		\
	};								\
	))								\
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
