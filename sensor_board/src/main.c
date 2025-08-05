/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#if defined(CONFIG_STORAGE)
#include <lib/storage.h>
#endif /* CONFIG_STORAGE */

#if defined(CONFIG_USB_SERIAL)
#include "usb_serial.h"
#endif /* CONFIG_USB_SERIAL */


LOG_MODULE_REGISTER(main, CONFIG_SENSOR_BOARD_LOG_LEVEL);

int main(void)
{
	int ret;

#if defined(CONFIG_USB_SERIAL)
	ret = init_usb_serial();
	if (ret) {
		LOG_ERR("Could not initialize USB Serial (%d)", ret);
		return 1;
	}
#endif /* CONFIG_USB_SERIAL */

	printk("Auxspace Sensor Board %s\n", APP_VERSION_STRING);

#if defined(CONFIG_STORAGE)
	ret = storage_init();
	if (ret) {
		LOG_ERR("Could not initialize storage (%d)", ret);
		return 1;
	}
#endif /* CONFIG_STORAGE */

	printk("Sensor board exiting.\n");
	return 0;
}
