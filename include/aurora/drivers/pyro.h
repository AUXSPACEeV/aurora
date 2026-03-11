/*
 * Copyright (c) 2019 Thomas Schmid <tom@lfence.de>
 * Copyright (c) 2024 Nordic Semiconductor ASA
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AURORA_DRIVERS_PYRO_H__
#define __AURORA_DRIVERS_PYRO_H__

#include "zephyr/toolchain.h"
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
 
/**
 * @defgroup drivers_pyro Pyro drivers
 * @ingroup drivers
 * @{
 *
 * @brief Driver class for rocketry pyro ignition.
 *
 * This driver class provides functions to assert multiple pyro channels,
 * arm and disarm pyro modules and (optionally) read pyro success states.
 */

/**
 * @defgroup drivers_pyro_ops Pyro driver operations
 * @{
 *
 * @brief Operations of the pyro driver class.
 */

/** @brief Pyro driver class operations. */
__subsystem struct pyro_driver_api {
	int (*arm)(const struct device *dev, uint32_t channel);          /**< Arm channel. */
	int (*disarm)(const struct device *dev, uint32_t channel);       /**< Disarm channel. */
	int (*secure)(const struct device *dev, uint32_t channel);       /**< Secure channel. */
	int (*trigger)(const struct device *dev, uint32_t channel);      /**< Trigger channel. */
	int (*charge)(const struct device *dev, uint32_t channel);       /**< Charge channel. */
	int (*sense)(const struct device *dev, uint32_t channel, uint32_t *val);    /**< Read sense. */
	int (*read_cap)(const struct device *dev, uint32_t channel, uint32_t *val); /**< Read cap voltage. */
	int (*get_nchannels)(const struct device *dev);                  /**< Get channel count. */
};

/**
 * @brief Like DEVICE_DT_DEFINE() with pyro specifics.
 *
 * @details Defines a device which implements the pyro API.
 *
 * @param node_id The devicetree node identifier.
 *
 * @param init_fn Name of the init function of the driver.
 *
 * @param pm_device PM device resources reference (NULL if device does not use
 * PM).
 *
 * @param data_ptr Pointer to the device's private data.
 *
 * @param cfg_ptr The address to the structure containing the configuration
 * information for this instance of the driver.
 *
 * @param level The initialization level. See SYS_INIT() for details.
 *
 * @param prio Priority within the selected initialization level. See
 * SYS_INIT() for details.
 *
 * @param api_ptr Provides an initial pointer to the API function struct used
 * by the driver. Can be NULL.
 */
#define PYRO_DEVICE_DT_DEFINE(node_id, init_fn, pm_device,		\
			      data_ptr, cfg_ptr, level, prio,		\
			      api_ptr, ...)				\
	DEVICE_DT_DEFINE(node_id, init_fn, pm_device,		\
			 data_ptr, cfg_ptr, level, prio,	\
			 api_ptr, __VA_ARGS__);

/**
 * @brief Like PYRO_DEVICE_DT_DEFINE() for an instance of a DT_DRV_COMPAT
 * compatible
 *
 * @param inst instance number. This is replaced by
 * <tt>DT_DRV_COMPAT(inst)</tt> in the call to PYRO_DEVICE_DT_DEFINE().
 *
 * @param ... other parameters as expected by PYRO_DEVICE_DT_DEFINE().
 */
#define PYRO_DEVICE_DT_INST_DEFINE(inst, ...)				\
	PYRO_DEVICE_DT_DEFINE(DT_DRV_INST(inst), __VA_ARGS__)

/**
 * @brief Arm a pyro channel.
 *
 * @param dev Pyro device instance.
 * @param channel Number of the channel to arm.
 *
 * @retval 0 if successful.
 * @retval -errno Other negative errno code on failure.
 */
__syscall int pyro_arm(const struct device *dev, uint32_t channel);

static inline int z_impl_pyro_arm(const struct device *dev, uint32_t channel)
{
__ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));

return DEVICE_API_GET(pyro, dev)->arm(dev, channel);
}

/**
 * @brief Disarm a pyro channel.
 *
 * @param dev Pyro device instance.
 * @param channel Number of the channel to disarm.
 *
 * @retval 0 if successful.
 * @retval -errno Other negative errno code on failure.
 */
__syscall int pyro_disarm(const struct device *dev, uint32_t channel);

static inline int z_impl_pyro_disarm(const struct device *dev, uint32_t channel)
{
__ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));

return DEVICE_API_GET(pyro, dev)->disarm(dev, channel);
}

/**
 * @brief Secure a pyro channel.
 *
 * @param dev Pyro device instance.
 * @param channel Number of the channel to secure.
 *
 * @retval 0 if successful.
 * @retval -errno Other negative errno code on failure.
 */
 __syscall int pyro_secure(const struct device *dev, uint32_t channel);

 static inline int z_impl_pyro_secure(const struct device *dev, uint32_t channel)
 {
 __ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));
 
 return DEVICE_API_GET(pyro, dev)->secure(dev, channel);
 }

/**
 * @brief Trigger a pyro channel.
 *
 * @param dev Pyro device instance.
 * @param channel Number of the channel to trigger.
 *
 * @retval 0 if successful.
 * @retval -EINVAL if @p channel can not be set.
 * @retval -errno Other negative errno code on failure.
 */
 __syscall int pyro_trigger_channel(const struct device *dev, uint32_t channel);

static inline int z_impl_pyro_trigger_channel(const struct device *dev,
					      uint32_t channel)
{
__ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));

return DEVICE_API_GET(pyro, dev)->trigger(dev, channel);
}

/**
 * @brief Charge a pyro channel's capacitor.
 *
 * @param dev Pyro device instance.
 * @param channel Number of the channel to charge.
 *
 * @retval 0 if successful.
 * @retval -EINVAL if @p channel can not be set.
 * @retval -errno Other negative errno code on failure.
 */
__syscall int pyro_charge_channel(const struct device *dev, uint32_t channel);

static inline int z_impl_pyro_charge_channel(const struct device *dev,
					     uint32_t channel)
{
__ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));

return DEVICE_API_GET(pyro, dev)->charge(dev, channel);
}

/**
 * @brief Read a pyro channel's "health".
 *
 * @param dev Pyro device instance.
 * @param channel Number of the channel to read the "sense".
 * @param[out] val Read value.
 *
 * @retval 0 if successful.
 * @retval -EINVAL if @p channel is invalid.
 * @retval -errno Other negative errno code on failure.
 */
__syscall int pyro_sense_channel(const struct device *dev, uint32_t channel,
				uint32_t *val);

static inline int z_impl_pyro_sense_channel(const struct device *dev,
					   uint32_t channel, uint32_t *val)
{
__ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));

return DEVICE_API_GET(pyro, dev)->sense(dev, channel, val);
}

/**
 * @brief Read a pyro channel's capacitor charge.
 *
 * @param dev Pyro device instance.
 * @param channel Number of the channel to read the capacitor voltage of.
 * @param[out] val Read value.
 *
 * @retval 0 if successful.
 * @retval -EINVAL if @p channel is invalid.
 * @retval -errno Other negative errno code on failure.
 */
 __syscall int pyro_read_cap_channel(const struct device *dev, uint32_t channel,
	uint32_t *val);

static inline int z_impl_pyro_read_cap_channel(const struct device *dev,
		   uint32_t channel, uint32_t *val)
{
__ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));

return DEVICE_API_GET(pyro, dev)->read_cap(dev, channel, val);
}

/**
 * @brief Get the number of pyro channels on this module.
 *
 * @param dev Pyro device instance.
 *
 * @return Number of channels on success.
 * @retval -errno Negative errno code on failure.
 */
 __syscall int pyro_get_nchannels(const struct device *dev);

static inline int z_impl_pyro_get_nchannels(const struct device *dev)
{
__ASSERT_NO_MSG(DEVICE_API_IS(pyro, dev));

return DEVICE_API_GET(pyro, dev)->get_nchannels(dev);
}

#include <syscalls/pyro.h>

/** @} */

/** @} */

#endif /* __AURORA_DRIVERS_PYRO_H__*/
