/**
 * @file flight_log_disk_auto_format.c
 * @brief Guarded auto-mkfs for the companion FAT volume.
 *
 * Runs once at boot, after Zephyr's fstab has tried to automount the
 * FAT volume on the SD card.  Verifies the disk end to end (init with
 * retries, sector geometry, raw region fits the card, probe sector
 * readable) and latches flight_log_online() for the arming interlock.
 * The card is preserved untouched only when it carries the flight-frame
 * magic at the raw-region offset AND the FAT volume mounts and accepts
 * writes; otherwise the disk is repartitioned so the FAT volume ends at
 * offset-bytes (from the flight-log-disk binding) and the raw flight-log
 * region [offset-bytes, offset-bytes+size-bytes) sits outside the FAT
 * partition where fs_mkfs() cannot reach it.
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
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_LOG_BACKEND_FS)
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#endif
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

/* 64-bit DT byte values are encoded as two 32-bit cells <HI LO>. */
#define DISK_OFFSET_BYTES                                                   \
	(((uint64_t)DT_PROP_BY_IDX(DISK_NODE, offset_bytes, 0) << 32) |     \
	 (uint64_t)DT_PROP_BY_IDX(DISK_NODE, offset_bytes, 1))

#define DISK_HAS_SIZE DT_NODE_HAS_PROP(DISK_NODE, size_bytes)
#define DISK_SIZE_BYTES                                                     \
	COND_CODE_1(DISK_HAS_SIZE,                                          \
		    ((((uint64_t)DT_PROP_BY_IDX(DISK_NODE, size_bytes, 0)   \
		       << 32) |                                             \
		      (uint64_t)DT_PROP_BY_IDX(DISK_NODE, size_bytes, 1))), \
		    (0ULL))

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

/* Result of probing the first sector of the raw flight-log region.
 * This answers only "is there flight data to preserve?"
 * It is deliberately independent of whether the FAT volume is healthy,
 * which is a separate question decided by fat_volume_mountable().
 */
enum raw_region_state {
	RAW_HAS_FLIGHT_DATA, /**< AURORA flight-frame magic present at the offset. */
	RAW_NO_FLIGHT_DATA,  /**< Readable and aligned, but no magic. */
	RAW_UNKNOWN,         /**< Misaligned or unreadable. Do not touch the card. */
};

static enum raw_region_state probe_raw_region(uint32_t sector_size)
{
	if ((DISK_OFFSET_BYTES % sector_size) != 0) {
		LOG_WRN("auto-mkfs: offset-bytes %llu not aligned to sector "
			"size %u. Leaving card untouched",
			(unsigned long long)DISK_OFFSET_BYTES, sector_size);
		return RAW_UNKNOWN;
	}

	uint32_t off_sec = (uint32_t)(DISK_OFFSET_BYTES / sector_size);

	int rc = disk_access_read(DISK_NAME, probe_buf, off_sec, 1);

	if (rc != 0) {
		LOG_WRN("auto-mkfs: probe read at sector %u failed (%d). "
			"Leaving card untouched", off_sec, rc);
		return RAW_UNKNOWN;
	}

	if (memcmp(probe_buf, AURORA_BIN_FRAME_MAGIC,
		   sizeof(AURORA_BIN_FRAME_MAGIC) - 1) == 0) {
		LOG_INF("auto-mkfs: flight magic at sector %u", off_sec);
		return RAW_HAS_FLIGHT_DATA;
	}

	return RAW_NO_FLIGHT_DATA;
}

/* True if the FAT volume is usable: either fstab already automounted it,
 * or an explicit mount succeeds now.  A blank or corrupt FAT returns false.
 * On success the volume is left mounted, which is exactly what the preserve
 * path wants.
 */
static bool fat_volume_mountable(struct fs_mount_t *mp)
{
	struct fs_statvfs stat;

	if (fs_statvfs(FS_MOUNT_POINT, &stat) == 0) {
		return true; /* fstab automounted it */
	}

	return fs_mount(mp) == 0;
}

/* A FAT volume can mount cleanly and still reject writes (corrupt FAT
 * chains, bad allocation metadata).  The current run needs a working FAT
 * for the FS log backend and post-flight CSV conversion, so demand a full
 * create/write/close/unlink round-trip before trusting the volume.
 */
static bool fat_volume_writable(void)
{
	static const char probe_path[] = FS_MOUNT_POINT "/.aurora_wp";
	struct fs_file_t f;
	uint8_t byte = 0xA5;
	bool ok;

	fs_file_t_init(&f);
	if (fs_open(&f, probe_path, FS_O_CREATE | FS_O_WRITE) != 0) {
		return false;
	}
	ok = (fs_write(&f, &byte, sizeof(byte)) == (ssize_t)sizeof(byte));
	ok = (fs_close(&f) == 0) && ok;
	(void)fs_unlink(probe_path);

	return ok;
}

/* SD cards coming out of power-up occasionally miss the first init
 * handshake ("card never left busy state") and succeed on a clean retry.
 * disk_access_init() only latches its refcount on success, so calling it
 * again after a failure re-runs the full card bring-up.
 */
#define DISK_INIT_ATTEMPTS       3
#define DISK_INIT_RETRY_DELAY_MS 250

static int disk_init_with_retry(void)
{
	int rc = -EIO;

	for (int attempt = 1; attempt <= DISK_INIT_ATTEMPTS; attempt++) {
		rc = disk_access_init(DISK_NAME);
		if (rc == 0) {
			return 0;
		}
		LOG_WRN("auto-mkfs: disk_access_init(%s) attempt %d/%d "
			"failed (%d)", DISK_NAME, attempt,
			DISK_INIT_ATTEMPTS, rc);
		if (attempt < DISK_INIT_ATTEMPTS) {
			k_sleep(K_MSEC(DISK_INIT_RETRY_DELAY_MS));
		}
	}

	return rc;
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
	 * System ID 0x06 (FAT16) is a placeholder.  fs_mkfs() rewrites
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

/* Health of the raw flight-log region, latched by
 * flight_log_disk_auto_format() from what the bring-up actually verified
 * (disk init, sector geometry, region fits the card, probe sector
 * readable) and published for the state machine's arming interlock.
 * Never set back to true after boot: a card that fails bring-up stays
 * locked out (arming inhibited) until the next reboot.  We never fly a
 * mission we cannot record.
 */
static atomic_t flight_log_online_flag = ATOMIC_INIT(0);

bool flight_log_online(void)
{
	return atomic_get(&flight_log_online_flag) != 0;
}

/* One full FAT rebuild pass: unmount (if mounted), rewrite the MBR,
 * mkfs partition 1, remount.  Returns 0 with the volume mounted, or a
 * negative errno from the first failing step.
 */
static int rebuild_fat_volume(struct fs_mount_t *mp, uint32_t sector_size,
			      uint32_t part_start, uint32_t fat_end_lba)
{
	struct fs_statvfs stat;
	int rc;

	/* fstab may have automounted an existing FAT in partition 1; we
	 * are about to rewrite the MBR underneath FatFs, so unmount first.
	 */
	if (fs_statvfs(FS_MOUNT_POINT, &stat) == 0) {
		rc = fs_unmount(mp);
		if (rc != 0) {
			LOG_ERR("auto-mkfs: fs_unmount(%s) failed (%d)",
				FS_MOUNT_POINT, rc);
			return rc;
		}
	}

	rc = write_mbr_partition_1(sector_size, part_start,
				   fat_end_lba - part_start);
	if (rc != 0) {
		LOG_ERR("auto-mkfs: MBR write failed (%d)", rc);
		return rc;
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
		return rc;
	}

	rc = fs_mount(mp);
	if (rc != 0) {
		LOG_ERR("auto-mkfs: fs_mount(%s) after mkfs failed (%d)",
			mp->mnt_point, rc);
		return rc;
	}

	return 0;
}

/* Number of full rebuild passes to attempt before giving up on the FAT.
 * A second pass rescues one-shot write hiccups; more would just delay
 * boot on a genuinely dead card.
 */
#define FAT_REBUILD_ATTEMPTS 2

/* Non-static so unit tests can re-invoke the entry point to simulate a
 * reboot without going through Zephyr's init system.  Production callers
 * should rely on the SYS_INIT registration below.
 */
int flight_log_disk_auto_format(void)
{
	struct fs_mount_t *mp = &FS_FSTAB_ENTRY(FS_NODE);
	uint32_t sector_size = 0;
	uint32_t sector_count = 0;
	int rc;

	/* Pessimistic default: everything below must succeed for the raw
	 * region to count as recordable.
	 */
	atomic_set(&flight_log_online_flag, 0);

	rc = disk_init_with_retry();
	if (rc != 0) {
		LOG_ERR("auto-mkfs: disk %s not initialisable (%d). No "
			"flight recording, arming inhibited", DISK_NAME, rc);
		return 0;
	}

	rc = disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE,
			       &sector_size);
	if (rc != 0 || sector_size == 0 || sector_size > PROBE_BUF_SZ) {
		LOG_ERR("auto-mkfs: GET_SECTOR_SIZE failed (%d, sz=%u). "
			"Refusing to format, arming inhibited",
			rc, sector_size);
		return 0;
	}

	/* The raw region must physically fit on this card.  A mismatch is a
	 * configuration error (e.g. a smaller card than the DT expects):
	 * bin_init() would reject it at ARM time anyway, so fail loudly now
	 * and keep arming inhibited instead of discovering it on the pad.
	 */
	rc = disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT,
			       &sector_count);
	if (rc != 0 || sector_count == 0) {
		LOG_ERR("auto-mkfs: GET_SECTOR_COUNT failed (%d, n=%u). "
			"Refusing to format, arming inhibited",
			rc, sector_count);
		return 0;
	}

	uint64_t disk_bytes = (uint64_t)sector_count * sector_size;

	if (DISK_OFFSET_BYTES + DISK_SIZE_BYTES > disk_bytes) {
		LOG_ERR("auto-mkfs: raw region [%llu..%llu) exceeds card "
			"capacity %llu. Wrong card for this DT? Arming "
			"inhibited",
			(unsigned long long)DISK_OFFSET_BYTES,
			(unsigned long long)(DISK_OFFSET_BYTES +
					     DISK_SIZE_BYTES),
			(unsigned long long)disk_bytes);
		return 0;
	}

	enum raw_region_state raw = probe_raw_region(sector_size);

	if (raw == RAW_UNKNOWN) {
		/* Geometry is fine but the probe sector is unreadable: the
		 * card cannot be trusted to record a flight.  Leave it
		 * untouched and keep arming inhibited.
		 */
		return 0;
	}

	/* From here on the raw region is verified recordable; FAT problems
	 * below no longer take the flight recorder offline.
	 */
	atomic_set(&flight_log_online_flag, 1);

	/* Preserve the card only when it carries recorded flight data AND
	 * the FAT volume is genuinely usable (mounts and accepts writes).
	 * A corrupt, unmountable or read-only FAT is rebuilt even with
	 * flight data present: partition 1 ends at offset-bytes and the raw
	 * flight-log region lies beyond it, so fs_mkfs() cannot reach the
	 * recorded data.  With no flight data we always reformat for a
	 * clean slate (fresh SD card / blank RAM disk in sim).
	 */
	if (raw == RAW_HAS_FLIGHT_DATA && fat_volume_mountable(mp) &&
	    fat_volume_writable()) {
		LOG_INF("auto-mkfs: flight data present and %s writable. "
			"Preserving", FS_MOUNT_POINT);
		return 0;
	}

	if (raw == RAW_HAS_FLIGHT_DATA) {
		LOG_WRN("auto-mkfs: flight data present but %s is not usable. "
			"Rebuilding FAT (raw flight region preserved)",
			FS_MOUNT_POINT);
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

	for (int attempt = 1; attempt <= FAT_REBUILD_ATTEMPTS; attempt++) {
		rc = rebuild_fat_volume(mp, sector_size, part_start,
					fat_end_lba);
		if (rc == 0) {
			LOG_INF("auto-mkfs: %s formatted and mounted (FAT in "
				"[%u..%u), raw region preserved)",
				mp->mnt_point, part_start, fat_end_lba);
			return 0;
		}
		LOG_WRN("auto-mkfs: rebuild attempt %d/%d failed (%d)",
			attempt, FAT_REBUILD_ATTEMPTS, rc);
	}

	/* FAT is dead but the raw region passed its checks: binary flight
	 * recording still works, only FS logging / on-card CSV conversion
	 * are lost.  Don't block boot and don't inhibit arming.
	 */
	LOG_ERR("auto-mkfs: giving up on %s. Continuing with raw flight "
		"recording only", FS_MOUNT_POINT);
	return 0;
}

#if defined(CONFIG_LOG_BACKEND_FS)
/*
 * Bring up the file-system log backend, but only once the FAT volume is
 * actually mounted.  With CONFIG_LOG_BACKEND_FS_AUTOSTART=n the backend stays
 * idle at boot, so the logging thread never flushes into an unmounted /MMC:
 * Safe to call on every path: if nothing is mounted, simply leave disk logging
 * off.
 */
static void flight_log_fs_backend_start(void)
{
	struct fs_statvfs stat;
	const struct log_backend *backend;
	int rc;

	if (fs_statvfs(FS_MOUNT_POINT, &stat) != 0) {
		LOG_WRN("fs log backend: %s not mounted. Disk logging disabled",
			FS_MOUNT_POINT);
		return;
	}

	/* fs_open() creates files, not their parent directory.  Probe with
	 * fs_stat() first: on a preserved volume the directory already
	 * exists, and calling fs_mkdir() anyway makes the fs subsystem log a
	 * scary (but harmless) -EEXIST error on every boot.
	 */
	struct fs_dirent entry;

	if (fs_stat(CONFIG_LOG_BACKEND_FS_DIR, &entry) != 0) {
		rc = fs_mkdir(CONFIG_LOG_BACKEND_FS_DIR);
		if (rc != 0 && rc != -EEXIST) {
			LOG_WRN("fs log backend: mkdir(%s) failed (%d). Disk "
				"logging disabled", CONFIG_LOG_BACKEND_FS_DIR,
				rc);
			return;
		}
	}

	backend = log_backend_get_by_name("log_backend_fs");
	if (backend == NULL) {
		LOG_WRN("fs log backend: backend not found");
		return;
	}

	log_backend_enable(backend, backend->cb->ctx, CONFIG_LOG_MAX_LEVEL);
	LOG_INF("fs log backend: enabled on %s", CONFIG_LOG_BACKEND_FS_DIR);
}
#else
static inline void flight_log_fs_backend_start(void) { }
#endif /* CONFIG_LOG_BACKEND_FS */

/*
 * SYS_INIT entry point: verify the flight-log disk and format the FAT
 * volume if needed (this latches flight_log_online()), then start the
 * file-system log backend now that /MMC: is (or isn't) mounted.
 */
static int flight_log_disk_bringup(void)
{
	(void)flight_log_disk_auto_format();
	flight_log_fs_backend_start();

	if (!flight_log_online()) {
		LOG_WRN("flight-log disk %s offline at boot. "
			"Arming will be inhibited until reboot", DISK_NAME);
	}
	return 0;
}

/* Run after fstab automount (APPLICATION level, default priority). */
SYS_INIT(flight_log_disk_bringup, APPLICATION, 99);
