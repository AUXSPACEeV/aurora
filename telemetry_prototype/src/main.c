/*
 * Copyright (c) 2024 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#if defined(CONFIG_STORAGE_SDCARD)
#include "sdcard.h"
#endif /* CONFIG_STORAGE_SDCARD */

#if defined(CONFIG_USB_SERIAL)
#include "usb_serial.h"
#endif /* CONFIG_USB_SERIAL */


LOG_MODULE_REGISTER(main, CONFIG_TELEMETRY_PROT_LOG_LEVEL);

#define BLINK_PERIOD_MS_STEP 100U
#define BLINK_PERIOD_MS_MAX  1000U

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

	printk("Zephyr Weather Station %s\n", APP_VERSION_STRING);

#if defined(CONFIG_STORAGE_SDCARD)
	ret = init_sd();
	if (ret) {
		LOG_ERR("Could not initialize SD card (%d)", ret);
		return 1;
	}
#endif /* CONFIG_STORAGE_SDCARD */

	printk("Telemetry prototype exiting.\n");
	return 0;
}
