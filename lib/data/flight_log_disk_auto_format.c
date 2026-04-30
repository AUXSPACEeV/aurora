/**
 * @file flight_log_disk_auto_format.c
 * @brief Guarded auto-mkfs for the companion FAT volume.
 *
 * Runs once at boot, after Zephyr's fstab has tried to automount the
 * FAT volume on the SD card.  The sole guard is the flight-frame magic
 * at the start of the raw flight-log region: if it is present, the
 * card is left untouched; otherwise the disk is repartitioned so the
 * FAT volume ends at offset-bytes (from the flight-log-disk binding)
 * and the raw flight-log region [offset-bytes, offset-bytes+size-bytes)
 * sits outside the FAT partition where fs_mkfs() cannot reach it.
 *
 * Repartitioning is done by writing a hand-built MBR (one partition
 * spanning [START_LBA, offset-bytes/sector_size)) and then calling
 * fs_mkfs() with FF_MULTI_PARTITION enabled, so FatFs forcibly formats
 * inside partition 1 instead of treating the whole disk as one volume.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <ff.h>

#include <aurora/lib/data_logger.h>

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

#if !DT_HAS_CHOSEN(auxspace_flight_log_disk)
#error "DATA_LOGGER_DISK_AUTO_MKFS requires DT chosen 'auxspace,flight-log-disk'."
#endif

#if !DT_HAS_CHOSEN(auxspace_ffs)
#error "DATA_LOGGER_DISK_AUTO_MKFS requires DT chosen 'auxspace,ffs' (the zephyr,fstab,fatfs entry)."
#endif

#define DISK_NODE         DT_CHOSEN(auxspace_flight_log_disk)
#define DISK_NAME         DT_PROP(DISK_NODE, disk_name)
#define DISK_OFFSET_BYTES ((uint64_t)DT_PROP(DISK_NODE, offset_bytes))

#define FS_NODE           DT_CHOSEN(auxspace_ffs)
#define FS_MOUNT_POINT    DT_PROP(FS_NODE, mount_point)

FS_FSTAB_DECLARE_ENTRY(FS_NODE);

/* The auto-mkfs module hard-wires VolToPart[] for the single-disk
 * case: the only logical drive maps to physical drive 0 (= the only
 * entry in Zephyr's auto-generated FF_VOLUME_STRS) and forced
 * partition 1.  Boards with multiple disk-access devices listed in
 * the FatFs volume table need their own VolToPart definition.
 */
BUILD_ASSERT(FF_VOLUMES == 1,
	     "DATA_LOGGER_DISK_AUTO_MKFS expects exactly one FatFs volume; "
	     "extend VolToPart[] in flight_log_disk_auto_format.c if you "
	     "have additional disk-access devices in DT.");

PARTITION VolToPart[FF_VOLUMES] = {
	{0, 1},
};

/* One sector of bounce buffer for the magic probe / MBR write.
 * Sized to FF_MAX_SS (4096) to cover any disk reporting a non-512
 * sector size; SD/MMC always reports 512 in practice.
 */
#define PROBE_BUF_SZ 4096
static uint8_t probe_buf[PROBE_BUF_SZ] __aligned(4);

/* MBR layout constants (ISO/IEC 9293; 512-byte sector). */
#define MBR_PART_TABLE_OFS 0x1BE
#define MBR_PART_ENTRY_SZ  16
#define MBR_SIG_OFS        510
#define MBR_SIG_LO         0x55
#define MBR_SIG_HI         0xAA

/* Start LBA of partition 1.  1 MiB alignment (2048 sectors at 512 B/s)
 * is the de-facto standard on SD/MMC, but small RAM disks in tests
 * may have offset-bytes only a few MiB into the disk.  Anything ≥ 1
 * is legal; sector 1 leaves the most room for the partition.
 */
#define PART1_ALIGNED_LBA 2048u
#define PART1_MIN_LBA     1u

static bool flight_data_present(uint32_t sector_size)
{
	if ((DISK_OFFSET_BYTES % sector_size) != 0) {
		LOG_WRN("auto-mkfs: offset-bytes %llu not aligned to sector "
			"size %u — refusing to format",
			(unsigned long long)DISK_OFFSET_BYTES, sector_size);
		return true;
	}

	uint32_t off_sec = (uint32_t)(DISK_OFFSET_BYTES / sector_size);

	int rc = disk_access_read(DISK_NAME, probe_buf, off_sec, 1);

	if (rc != 0) {
		LOG_WRN("auto-mkfs: probe read at sector %u failed (%d) — "
			"refusing to format", off_sec, rc);
		return true;
	}

	if (memcmp(probe_buf, AURORA_BIN_FRAME_MAGIC,
		   sizeof(AURORA_BIN_FRAME_MAGIC) - 1) == 0) {
		LOG_INF("auto-mkfs: flight magic at sector %u — preserving",
			off_sec);
		return true;
	}

	return false;
}

static void mbr_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = (v >> 24) & 0xFF;
}

static int write_mbr_partition_1(uint32_t sector_size, uint32_t start_lba,
				 uint32_t size_lba)
{
	memset(probe_buf, 0, sector_size);

	uint8_t *pte = probe_buf + MBR_PART_TABLE_OFS;

	/* Status: not bootable.  CHS fields are ignored by modern OSes
	 * when LBA fields are set; fill with 0xFF as a "use LBA" marker.
	 * System ID 0x06 (FAT16) is a placeholder — fs_mkfs() rewrites
	 * this byte to the correct FAT type once the volume is laid out.
	 */
	pte[0] = 0x00;
	pte[1] = 0xFF; pte[2] = 0xFF; pte[3] = 0xFF;
	pte[4] = 0x06;
	pte[5] = 0xFF; pte[6] = 0xFF; pte[7] = 0xFF;
	mbr_put_le32(pte + 8, start_lba);
	mbr_put_le32(pte + 12, size_lba);

	probe_buf[MBR_SIG_OFS]     = MBR_SIG_LO;
	probe_buf[MBR_SIG_OFS + 1] = MBR_SIG_HI;

	return disk_access_write(DISK_NAME, probe_buf, 0, 1);
}

static int flight_log_disk_auto_format(void)
{
	struct fs_mount_t *mp = &FS_FSTAB_ENTRY(FS_NODE);
	uint32_t sector_size = 0;
	int rc;

	rc = disk_access_init(DISK_NAME);
	if (rc != 0) {
		LOG_WRN("auto-mkfs: disk_access_init(%s) failed (%d) — "
			"refusing to format", DISK_NAME, rc);
		return 0;
	}

	rc = disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE,
			       &sector_size);
	if (rc != 0 || sector_size == 0 || sector_size > PROBE_BUF_SZ) {
		LOG_WRN("auto-mkfs: GET_SECTOR_SIZE failed (%d, sz=%u) — "
			"refusing to format", rc, sector_size);
		return 0;
	}

	if (flight_data_present(sector_size)) {
		LOG_INF("auto-mkfs: flight data present — preserving %s",
			FS_MOUNT_POINT);
		return 0;
	}

	uint32_t fat_end_lba = (uint32_t)(DISK_OFFSET_BYTES / sector_size);
	uint32_t part_start  = (fat_end_lba > PART1_ALIGNED_LBA * 2)
				       ? PART1_ALIGNED_LBA
				       : PART1_MIN_LBA;

	if (fat_end_lba <= part_start) {
		LOG_ERR("auto-mkfs: offset-bytes %llu too small for a FAT "
			"partition (fat_end_lba=%u, part_start=%u)",
			(unsigned long long)DISK_OFFSET_BYTES, fat_end_lba,
			part_start);
		return 0;
	}

	uint32_t part_size = fat_end_lba - part_start;

	/* fstab may have automounted an existing FAT in partition 1; we
	 * are about to rewrite the MBR underneath FatFs, so unmount first.
	 */
	struct fs_statvfs stat;
	bool was_mounted = (fs_statvfs(FS_MOUNT_POINT, &stat) == 0);

	if (was_mounted) {
		rc = fs_unmount(mp);
		if (rc != 0) {
			LOG_ERR("auto-mkfs: fs_unmount(%s) failed (%d) — "
				"refusing to format", FS_MOUNT_POINT, rc);
			return 0;
		}
	}

	rc = write_mbr_partition_1(sector_size, part_start, part_size);
	if (rc != 0) {
		LOG_ERR("auto-mkfs: MBR write failed (%d)", rc);
		return 0;
	}

	LOG_WRN("auto-mkfs: formatting %s partition 1 [%u..%u) on %s",
		FS_MOUNT_POINT, part_start, fat_end_lba, DISK_NAME);

	const char *dev = mp->mnt_point;

	/* fatfs_mkfs takes the f_mkfs() path string (e.g. "MMC:"), which is
	 * the mount point without the leading slash.
	 */
	if (dev[0] == '/') {
		dev++;
	}

	rc = fs_mkfs(FS_FATFS, (uintptr_t)dev, NULL, 0);
	if (rc != 0) {
		LOG_ERR("auto-mkfs: fs_mkfs(%s) failed (%d)", dev, rc);
		return 0; /* Don't block boot. */
	}

	rc = fs_mount(mp);
	if (rc != 0) {
		LOG_ERR("auto-mkfs: fs_mount(%s) after mkfs failed (%d)",
			mp->mnt_point, rc);
		return 0;
	}

	LOG_INF("auto-mkfs: %s formatted and mounted (FAT in [%u..%u), raw "
		"region preserved)", mp->mnt_point, part_start, fat_end_lba);
	return 0;
}

/* Run after fstab automount (APPLICATION level, default priority). */
SYS_INIT(flight_log_disk_auto_format, APPLICATION, 99);
