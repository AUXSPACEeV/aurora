/**
 * @file bus_unwedge.c
 * @brief Clock a stuck I2C bus clean before the sensor drivers probe it.
 *
 * bus_recover.h handles a bus that wedges while the board is flying.  It
 * cannot handle a bus that is already wedged when the board boots, because
 * nothing gets far enough to call it: the sensor driver's own init fails,
 * Zephyr marks the device permanently not-ready, and baro_init()/imu_init()
 * return before the fetch path that carries the recovery is ever reached.
 * Device init runs exactly once, so the sensor is gone for the whole flight.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bus_unwedge, CONFIG_AURORA_SENSORS_LOG_LEVEL);

/* Resolved the same way the fetch-path recovery resolves them, so the two
 * always act on the same controllers. */
#if DT_HAS_CHOSEN(auxspace_imu) && DT_ON_BUS(DT_CHOSEN(auxspace_imu), i2c)
#define IMU_I2C_BUS DEVICE_DT_GET(DT_BUS(DT_CHOSEN(auxspace_imu)))
#endif

#if DT_HAS_CHOSEN(auxspace_baro) && DT_ON_BUS(DT_CHOSEN(auxspace_baro), i2c)
#define BARO_I2C_BUS DEVICE_DT_GET(DT_BUS(DT_CHOSEN(auxspace_baro)))
#endif

#if defined(IMU_I2C_BUS) || defined(BARO_I2C_BUS)

static const struct device *const sensor_buses[] = {
#if defined(IMU_I2C_BUS)
	IMU_I2C_BUS,
#endif
#if defined(BARO_I2C_BUS) && \
	(!defined(IMU_I2C_BUS) || \
	 !DT_SAME_NODE(DT_BUS(DT_CHOSEN(auxspace_imu)), \
		       DT_BUS(DT_CHOSEN(auxspace_baro))))
	BARO_I2C_BUS,
#endif
};

static int sensor_bus_unwedge(void)
{
	ARRAY_FOR_EACH(sensor_buses, i) {
		const struct device *bus = sensor_buses[i];
		int rc;

		if (!device_is_ready(bus)) {
			LOG_ERR("%s not ready; cannot clear the bus before the "
				"sensors probe it", bus->name);
			continue;
		}

		rc = i2c_recover_bus(bus);
		if (rc != 0) {
			LOG_WRN("%s still wedged after recovery (%d); sensor "
				"init is expected to fail", bus->name, rc);
		} else {
			LOG_DBG("%s clear", bus->name);
		}
	}

	return 0;
}

/* Between I2C_INIT_PRIORITY and SENSOR_INIT_PRIORITY: the controller must
 * exist to be recovered, and the recovery is worthless after the sensors have
 * already had their one attempt at init. */
BUILD_ASSERT(CONFIG_AURORA_SENSOR_BUS_UNWEDGE_PRIORITY >
		     CONFIG_I2C_INIT_PRIORITY &&
	     CONFIG_AURORA_SENSOR_BUS_UNWEDGE_PRIORITY <
		     CONFIG_SENSOR_INIT_PRIORITY,
	     "bus unwedge must run after the I2C controller and before the sensors");

SYS_INIT(sensor_bus_unwedge, POST_KERNEL,
	 CONFIG_AURORA_SENSOR_BUS_UNWEDGE_PRIORITY);

#endif /* IMU_I2C_BUS || BARO_I2C_BUS */
