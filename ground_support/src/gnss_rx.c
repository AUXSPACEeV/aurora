/**
 * @file gnss_rx.c
 * @brief GNSS fix receiver: publishes each fix into the shared snapshot.
 *
 * The u-blox NEO-6M speaks plain NMEA, decoded by Zephyr's generic NMEA
 * driver. The driver owns the UART and its own worker; all we do is register
 * a data callback and translate the fix into the ground-station snapshot.
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_rx.h"
#include "ground_state.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gnss_rx, CONFIG_GROUND_SUPPORT_LOG_LEVEL);

#define GNSS_NODE DT_ALIAS(gnss)
BUILD_ASSERT(DT_NODE_HAS_STATUS(GNSS_NODE, okay),
	     "the 'gnss' alias must point at an enabled GNSS node");
#define GNSS_DEV DEVICE_DT_GET(GNSS_NODE)

/* Fixed-point wire units from struct navigation_data. */
#define NANODEG_PER_DEG 1e9
#define MM_PER_M        1000.0

/**
 * @brief GNSS data callback (runs on the driver's worker).
 *
 * Fires on every decoded fix message, with or without a position lock, so the
 * satellite count and fix status stay live while acquiring; @c valid gates the
 * position itself.
 */
static void gnss_cb(const struct device *dev, const struct gnss_data *data)
{
	ARG_UNUSED(dev);

	const struct navigation_data *nav = &data->nav_data;
	struct gs_gnss g = {
		.valid = data->info.fix_status != GNSS_FIX_STATUS_NO_FIX,
		.fix = (uint8_t)data->info.fix_status,
		.sats = data->info.satellites_cnt,
		.lat_deg = (double)nav->latitude / NANODEG_PER_DEG,
		.lon_deg = (double)nav->longitude / NANODEG_PER_DEG,
		.alt_m = (double)nav->altitude / MM_PER_M,
		.speed_ms = (double)nav->speed / MM_PER_M,
	};

	gs_set_gnss(&g);
}

GNSS_DATA_CALLBACK_DEFINE(GNSS_DEV, gnss_cb);

int gnss_rx_init(void)
{
	const struct device *dev = GNSS_DEV;

	if (!device_is_ready(dev)) {
		LOG_ERR("GNSS device %s not ready", dev->name);
		return -ENODEV;
	}

	LOG_INF("GNSS ready: %s", dev->name);
	return 0;
}
