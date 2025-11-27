/*
 * Copyright (c) 2025, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ff.h>

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <lib/storage.h>

LOG_MODULE_REGISTER(simple_storage, CONFIG_SENSOR_BOARD_LOG_LEVEL);

#define FS_RET_OK	FR_OK

#define MMC_NODE DT_CHOSEN(auxspace_mmc)
#define MMC_DISK_NAME DT_PROP(MMC_NODE, disk_name)

#define AUTOMOUNT_NODE DT_CHOSEN(auxspace_ffs)
FS_FSTAB_DECLARE_ENTRY(AUTOMOUNT_NODE);

/* List dir entry by path
 *
 * @param path Absolute path to list
 *
 * @return Negative errno code on error, number of listed entries on
 *		   success.
 */
static int lsdir(const char *path)
{
	int res;
	struct fs_dir_t dirp;
	static struct fs_dirent entry;
	int count = 0;

	fs_dir_t_init(&dirp);

	/* Verify fs_opendir() */
	res = fs_opendir(&dirp, path);
	if (res) {
		LOG_ERR("Error opening dir %s [%d]\n", path, res);
		return res;
	}

	LOG_INF("Listing dir %s ...\n", path);
	for (;;) {
		/* Verify fs_readdir() */
		res = fs_readdir(&dirp, &entry);

		/* entry.name[0] == 0 means end-of-dir */
		if (res || entry.name[0] == 0) {
			break;
		}

		if (entry.type == FS_DIR_ENTRY_DIR) {
			LOG_INF("[DIR ] %s\n", entry.name);
		} else {
			LOG_INF("[FILE] %s (size = %zu)\n",
				entry.name, entry.size);
		}
		count++;
	}

	/* Verify fs_closedir() */
	fs_closedir(&dirp);
	if (res == 0) {
		res = count;
	}

	return res;
}

int init_sd()
{
	struct fs_mount_t *mp;
	int entries;

	/* raw disk i/o */
	uint64_t memory_size_mb;
	uint32_t block_count;
	uint32_t block_size;

	if (disk_access_ioctl(MMC_DISK_NAME,
			DISK_IOCTL_CTRL_INIT, NULL) != 0) {
		LOG_ERR("Storage init ERROR!");
		return -EIO;
	}

	if (disk_access_ioctl(MMC_DISK_NAME ,
			DISK_IOCTL_GET_SECTOR_COUNT, &block_count)) {
		LOG_ERR("Unable to get sector count");
		return -EIO;
	}
	LOG_INF("Block count %u", block_count);

	if (disk_access_ioctl(MMC_DISK_NAME,
			DISK_IOCTL_GET_SECTOR_SIZE, &block_size)) {
		LOG_ERR("Unable to get sector size");
		return -EIO;
	}
	LOG_INF("Sector size %u\n", block_size);

	memory_size_mb = (uint64_t)block_count * block_size;
	LOG_INF("Memory Size(MB) %u\n", (uint32_t)(memory_size_mb >> 20));

	mp = &FS_FSTAB_ENTRY(AUTOMOUNT_NODE);

	entries = lsdir(mp->mnt_point);
	LOG_INF("%d entries on drive.", entries);

	if (disk_access_ioctl(MMC_DISK_NAME,
		DISK_IOCTL_CTRL_DEINIT, NULL) != 0) {
		LOG_ERR("Storage deinit ERROR!");
		return -EIO;
	}

	return entries < 0 ? -EIO : 0;
}

int storage_init()
{
	return init_sd();
}
