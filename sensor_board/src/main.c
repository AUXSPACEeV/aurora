/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#if defined(CONFIG_STORAGE)
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <lib/storage.h>

#define LOGFILE_NAME 		"events.log"
#define LOGFILE_NAME_LEN	MAX(sizeof(LOGFILE_NAME), strlen(DISK_MOUNT_PT))
#endif /* CONFIG_STORAGE */

#if defined(CONFIG_USB_SERIAL)
#include <lib/usb_serial.h>
#endif /* CONFIG_USB_SERIAL */

#if defined(CONFIG_IMU)
#include <lib/imu.h>
#endif /* CONFIG_IMU */

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

	LOG_INF("Auxspace Sensor Board %s\n", APP_VERSION_STRING);

#if defined(CONFIG_STORAGE)
	ret = storage_init();
	if (ret) {
		LOG_ERR("Could not initialize storage (%d)", ret);
		return 1;
	}

	char path[MAX_PATH];
	struct fs_file_t file;
	struct fs_dir_t dir;
	int base = strlen(DISK_MOUNT_PT);

	fs_file_t_init(&file);
	fs_dir_t_init(&dir);

	if (base >= (sizeof(path) - LOGFILE_NAME_LEN)) {
		LOG_ERR("Not enough concatenation buffer to create file paths");
		return -EOF;
	}

	LOG_INF("Creating some dir entries in %s", DISK_MOUNT_PT);
	strncpy(path, DISK_MOUNT_PT, sizeof(path));

	path[base++] = '/';
	path[base] = 0;
	strcat(&path[base], LOGFILE_NAME);

	if (fs_open(&file, path, FS_O_CREATE) != 0) {
		LOG_ERR("Failed to create file %s", path);
		return -EBADF;
	}
	fs_close(&file);

	path[base] = 0;
	strcat(&path[base], "cache");

	ret = fs_opendir(&dir, path);
	if (ret) {
		if (fs_mkdir(path) != 0) {
			LOG_ERR("Failed to create dir %s", path);
			/* If code gets here, it has at least successes to create the
			 * file so allow function to return true.
			 */
			return -ENOENT;
		}
		return ret;
	}

#endif /* CONFIG_STORAGE */

#if defined(CONFIG_IMU)
	const struct device *const imu = DEVICE_DT_GET_ONE(st_lsm6dso32);
	if (imu_init(imu)) {
		LOG_ERR("Could not initialize IMU: %d", ret);
	}
#if !defined(CONFIG_LSM6DSO_TRIGGER)
	float imu_hz = strtof(CONFIG_IMU_HZ, NULL);
	for(;;) {
		int rc = imu_poll(imu);
		(void) rc;
		k_sleep(K_MSEC((int)(1000 / imu_hz)));
	}
#endif /* CONFIG_LSM6DSO_TRIGGER */
#endif /* CONFIG_IMU */

	LOG_INF("Sensor board exiting.\n");
	return 0;
}
