/*
 * Copyright (c) 2024 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#if defined(CONFIG_STORAGE_SDCARD)
#include "io/storage/sdcard.h"
#endif /* CONFIG_STORAGE_SDCARD */

LOG_MODULE_REGISTER(main, CONFIG_TELEMETRY_PROT_LOG_LEVEL);

#define BLINK_PERIOD_MS_STEP 100U
#define BLINK_PERIOD_MS_MAX  1000U

int main(void)
{
	printk("Zephyr Weather Station %s\n", APP_VERSION_STRING);

#if defined(CONFIG_STORAGE_SDCARD)
	init_sd();
#endif /* CONFIG_STORAGE_SDCARD */
	return 0;
}
