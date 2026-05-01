/**
 * @file main.c
 * @brief Unit tests for the disk-backed flight-log auto-mkfs guard.
 *
 * The auto-mkfs module (aurora/lib/data/flight_log_disk_auto_format.c)
 * coexists a FAT volume and a raw flight-log region on a single
 * disk-access device.  At boot, after Zephyr's fstab has tried to
 * automount the FAT volume, the module reads the first sector of the
 * raw region, looks for the AURORA flight-frame magic, and either
 * preserves the disk untouched or rewrites the MBR plus reformats the
 * FAT partition so that the raw region structurally lies outside any
 * filesystem.
 *
 * Two suites cover the two arms of that guard:
 *
 *  1. **disk_auto_mkfs_blank** — exercises the format-on-mismatch path.
 *     The simulator brings up a blank RAM disk, Zephyr's automount fails
 *     (no FAT yet), the auto-mkfs SYS_INIT hook then writes a fresh MBR
 *     and calls fs_mkfs(), and the FatFS volume is mounted before the
 *     first test runs.  The suite asserts that the volume is mounted
 *     and that ordinary file I/O succeeds, end to end.
 *
 *  2. **disk_auto_mkfs_preserve** — exercises the preservation path.
 *     The test seeds the configured raw-region offset with the AURORA
 *     flight-frame magic and re-invokes the auto-format entry point
 *     directly to mimic a reboot from a populated card.  The guard must
 *     short-circuit on the magic, leave the raw bytes intact, and leave
 *     the FAT volume mounted.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>

#include <zephyr/ztest.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>

#include <aurora/lib/data_logger.h>

#define DISK_NODE         DT_CHOSEN(auxspace_flight_log_disk)
#define DISK_NAME         DT_PROP(DISK_NODE, disk_name)
#define DISK_OFFSET_BYTES                                                   \
	(((uint64_t)DT_PROP_BY_IDX(DISK_NODE, offset_bytes, 0) << 32) |     \
	 (uint64_t)DT_PROP_BY_IDX(DISK_NODE, offset_bytes, 1))

#define FS_MOUNT_POINT    "/RAM:"
#define SENTINEL_PATH     FS_MOUNT_POINT "/sentinel.txt"
#define SENTINEL_PAYLOAD  "auto-mkfs-ok"

/* Re-entry point exported by the auto-mkfs module.  Production callers
 * use the SYS_INIT hook; tests call this directly to simulate a reboot.
 */
extern int flight_log_disk_auto_format(void);

/* One-sector scratch buffer for raw disk_access_read/write probes. */
static uint8_t scratch[512];

static uint32_t sector_size_bytes(void)
{
	uint32_t sz = 0;

	zassert_ok(disk_access_ioctl(DISK_NAME,
				     DISK_IOCTL_GET_SECTOR_SIZE, &sz), NULL);
	zassert_true(sz != 0 && sz <= sizeof(scratch),
		     "Unexpected sector size %u (scratch is %zu bytes)",
		     sz, sizeof(scratch));
	return sz;
}

/* ========================================================================== */
/*  Suite 1: format-on-mismatch (blank disk → mounted FAT)                    */
/* ========================================================================== */

ZTEST_SUITE(disk_auto_mkfs_blank, NULL, NULL, NULL, NULL, NULL);

/**
 * @brief A blank RAM disk is reformatted at SYS_INIT and the FAT volume
 *        is mounted before the first test executes.
 *
 * The presence of a usable f_blocks count on @ref FS_MOUNT_POINT is the
 * primary signal: it means fstab automount (which fails on a blank disk)
 * was followed by the auto-mkfs hook reformatting and remounting the
 * volume.
 */
ZTEST(disk_auto_mkfs_blank, test_blank_disk_mounted_after_boot)
{
	struct fs_statvfs vfs;

	zassert_ok(fs_statvfs(FS_MOUNT_POINT, &vfs),
		   "auto-mkfs at SYS_INIT must leave " FS_MOUNT_POINT
		   " mounted");
	zassert_true(vfs.f_blocks > 0,
		     "FAT volume must report a non-zero block count");
}

/**
 * @brief The freshly formatted FAT volume accepts ordinary file I/O.
 *
 * Writes a small sentinel file, closes it, reopens it for read, and
 * verifies the payload round-trips byte-for-byte.  This catches cases
 * where fs_mkfs() succeeded but produced a structurally broken volume
 * that f_open / f_write would later reject.
 */
ZTEST(disk_auto_mkfs_blank, test_blank_disk_filesystem_writable)
{
	struct fs_file_t f;
	char readback[sizeof(SENTINEL_PAYLOAD)] = {0};

	(void)fs_unlink(SENTINEL_PATH);

	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, SENTINEL_PATH,
			   FS_O_CREATE | FS_O_WRITE), NULL);
	zassert_equal(fs_write(&f, SENTINEL_PAYLOAD,
			       sizeof(SENTINEL_PAYLOAD) - 1),
		      (ssize_t)sizeof(SENTINEL_PAYLOAD) - 1, NULL);
	zassert_ok(fs_close(&f), NULL);

	fs_file_t_init(&f);
	zassert_ok(fs_open(&f, SENTINEL_PATH, FS_O_READ), NULL);
	zassert_equal(fs_read(&f, readback, sizeof(readback) - 1),
		      (ssize_t)sizeof(SENTINEL_PAYLOAD) - 1, NULL);
	zassert_ok(fs_close(&f), NULL);

	zassert_str_equal(readback, SENTINEL_PAYLOAD,
			  "Sentinel payload must round-trip through the "
			  "freshly formatted FAT volume");
}

/* ========================================================================== */
/*  Suite 2: preservation (magic at offset → no reformat across "reboot")     */
/* ========================================================================== */

ZTEST_SUITE(disk_auto_mkfs_preserve, NULL, NULL, NULL, NULL, NULL);

/**
 * @brief A re-invocation of the auto-format entry point with the AURORA
 *        flight-frame magic present at the raw-region offset must leave
 *        the magic intact and leave the FAT volume mounted.
 *
 * The test mimics a reboot from a populated card: it writes the magic
 * into the first sector of the raw region (which lies outside the FAT
 * partition by construction), calls @ref flight_log_disk_auto_format
 * again, and asserts both that the magic survives the call and that the
 * mounted filesystem is still usable.  This is the only path that
 * preserves recorded flight data on the next boot, so it is intentionally
 * the strictest of the two suites: any silent reformat here would
 * destroy a flight log on a real card.
 */
ZTEST(disk_auto_mkfs_preserve, test_magic_survives_reentry)
{
	uint32_t sector_size = sector_size_bytes();
	uint32_t off_sec = (uint32_t)(DISK_OFFSET_BYTES / sector_size);

	memset(scratch, 0, sector_size);
	memcpy(scratch, AURORA_BIN_FRAME_MAGIC,
	       sizeof(AURORA_BIN_FRAME_MAGIC) - 1);
	zassert_ok(disk_access_write(DISK_NAME, scratch, off_sec, 1),
		   "seeding the magic at sector %u must succeed", off_sec);

	zassert_ok(flight_log_disk_auto_format(),
		   "auto-format re-entry must return success when magic "
		   "is present");

	memset(scratch, 0, sector_size);
	zassert_ok(disk_access_read(DISK_NAME, scratch, off_sec, 1), NULL);
	zassert_mem_equal(scratch, AURORA_BIN_FRAME_MAGIC,
			  sizeof(AURORA_BIN_FRAME_MAGIC) - 1,
			  "Flight magic must survive an auto-format re-entry");

	struct fs_statvfs vfs;

	zassert_ok(fs_statvfs(FS_MOUNT_POINT, &vfs),
		   FS_MOUNT_POINT " must remain mounted across the "
		   "preservation re-entry");
}
