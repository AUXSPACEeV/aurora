/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The SD-card recorder.
 *
 * Every frame the link hands over is written, unconditionally: there is
 * no state machine, no arming and no flight window here, because on the
 * ground the interesting part of a flight is exactly the part a gate
 * would have to guess at. The recorder starts at boot and keeps going
 * until power is cut.
 *
 * A recording is two files sharing an index in CONFIG_TELEMETRY_LOGGER_DIR:
 *
 *   RX_<n>.BIN  the frames verbatim, back to back - the same byte stream
 *               abby put on the stack UART, so any tool that parses the
 *               wire format parses the file. This is the authoritative
 *               record.
 *   RX_<n>.CSV  one decoded row per frame, prefixed with the time the
 *               frame arrived here. Convenience only.
 *
 * The recorder owns the mount. With no card, an unreadable card or after
 * a write error it unmounts, retries every CONFIG_TELEMETRY_LOGGER_RETRY_MS
 * and opens a fresh index rather than appending to a file whose tail may
 * be damaged. Frames that arrive with nothing open are counted, not
 * queued: once the queue is full the freshest frames would be the ones
 * lost, which is the wrong way round for telemetry.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <aurora/lib/telemetry/wire.h>

#include "recorder.h"

LOG_MODULE_REGISTER(tlog_recorder, CONFIG_TELEMETRY_LOGGER_LOG_LEVEL);

#if !DT_HAS_CHOSEN(auxspace_ffs)
#error "Set the auxspace,ffs chosen node to the zephyr,fstab entry to record to"
#endif

#define FS_NODE DT_CHOSEN(auxspace_ffs)

FS_FSTAB_DECLARE_ENTRY(FS_NODE);

#define FILE_PREFIX "RX_"
#define PATH_MAX_LEN 64

struct rx_frame {
	int64_t rx_ms;
	uint16_t len;
	uint8_t data[AURORA_TELEMETRY_WIRE_MAX_FRAME];
};

K_MSGQ_DEFINE(frame_q, sizeof(struct rx_frame),
	      CONFIG_TELEMETRY_LOGGER_QUEUE_DEPTH, 4);

static struct {
	atomic_t recorded;
	atomic_t dropped;
	atomic_t unrecorded;
	atomic_t write_errs;
	atomic_t sessions;
	atomic_t open;
	atomic_t index;
} stats;

/* Only touched by the recorder thread, apart from the snapshot in
 * recorder_get_stats(), which may read a torn value on a 32-bit core and
 * is only ever printed.
 */
static uint64_t bin_bytes;

/* SD activity LED. The notify library has the same thing, but it brings
 * the flight state machine with it, which this application has no use for.
 */
#if DT_HAS_CHOSEN(auxspace_disk_led)
#define DISK_LED_HOLD_MS 50

static const struct gpio_dt_spec disk_led =
	GPIO_DT_SPEC_GET(DT_CHOSEN(auxspace_disk_led), gpios);

static void disk_led_off(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_dt(&disk_led, 0);
}

static K_WORK_DELAYABLE_DEFINE(disk_led_work, disk_led_off);

static void disk_led_init(void)
{
	if (gpio_is_ready_dt(&disk_led)) {
		(void)gpio_pin_configure_dt(&disk_led, GPIO_OUTPUT_INACTIVE);
	}
}

static void disk_led_activity(void)
{
	(void)gpio_pin_set_dt(&disk_led, 1);
	(void)k_work_reschedule(&disk_led_work, K_MSEC(DISK_LED_HOLD_MS));
}
#else
static void disk_led_init(void) { }
static void disk_led_activity(void) { }
#endif /* DT_HAS_CHOSEN(auxspace_disk_led) */

static bool mounted;
static struct fs_file_t bin_file;
#if defined(CONFIG_TELEMETRY_LOGGER_CSV)
static struct fs_file_t csv_file;
#endif

int recorder_submit(const uint8_t *frame, size_t len)
{
	struct rx_frame f;

	if (!frame || len == 0 || len > sizeof(f.data)) {
		return -EINVAL;
	}

	f.rx_ms = k_uptime_get();
	f.len = (uint16_t)len;
	memcpy(f.data, frame, len);

	if (k_msgq_put(&frame_q, &f, K_NO_WAIT) != 0) {
		atomic_inc(&stats.dropped);
		return -ENOBUFS;
	}

	return 0;
}

/* ---- mount and session ------------------------------------------------ */

static int mount_card(void)
{
	struct fs_mount_t *mp = &FS_FSTAB_ENTRY(FS_NODE);
	int rc;

	if (mounted) {
		return 0;
	}

	rc = fs_mount(mp);
	if (rc == -EBUSY) {
		/* Automounted at boot. */
		rc = 0;
	}
	if (rc) {
		return rc;
	}

	mounted = true;
	LOG_INF("%s mounted", mp->mnt_point);
	return 0;
}

static void unmount_card(void)
{
	if (!mounted) {
		return;
	}

	/* Unmounting deinitialises the disk, so the next mount re-probes it
	 * and picks up a card that was swapped in the meantime.
	 */
	(void)fs_unmount(&FS_FSTAB_ENTRY(FS_NODE));
	mounted = false;
}

static int ensure_dir(void)
{
	struct fs_dirent ent;
	int rc = fs_stat(CONFIG_TELEMETRY_LOGGER_DIR, &ent);

	if (rc == 0) {
		return (ent.type == FS_DIR_ENTRY_DIR) ? 0 : -ENOTDIR;
	}
	if (rc != -ENOENT) {
		return rc;
	}

	return fs_mkdir(CONFIG_TELEMETRY_LOGGER_DIR);
}

/* Next free index: one past the highest RX_<n>.BIN already in the
 * directory, so a recording is never overwritten and a gap left by a
 * deleted file is not reused.
 */
static int next_index(uint32_t *out)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	uint32_t next = 0;
	int rc;

	fs_dir_t_init(&dir);

	rc = fs_opendir(&dir, CONFIG_TELEMETRY_LOGGER_DIR);
	if (rc) {
		return rc;
	}

	while ((rc = fs_readdir(&dir, &ent)) == 0 && ent.name[0] != '\0') {
		unsigned int n;
		char ext[4];

		if (ent.type != FS_DIR_ENTRY_FILE) {
			continue;
		}
		if (sscanf(ent.name, FILE_PREFIX "%u.%3s", &n, ext) == 2 &&
		    strcmp(ext, "BIN") == 0 && n + 1 > next) {
			next = n + 1;
		}
	}

	(void)fs_closedir(&dir);

	*out = next;
	return rc;
}

static int open_file(struct fs_file_t *file, uint32_t index, const char *ext)
{
	char path[PATH_MAX_LEN];

	snprintf(path, sizeof(path), "%s/" FILE_PREFIX "%04u.%s",
		 CONFIG_TELEMETRY_LOGGER_DIR, index, ext);

	fs_file_t_init(file);

	/* No FS_O_TRUNC: the index was free a moment ago, and if it somehow
	 * is not, appending loses less than truncating would.
	 */
	int rc = fs_open(file, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);

	if (rc) {
		LOG_ERR("open %s failed (%d)", path, rc);
	}
	return rc;
}

#if defined(CONFIG_TELEMETRY_LOGGER_CSV)
static const char csv_header[] =
	"rx_ms,type,len,tx_ms,state,armed,sm_type,flags,"
	"altitude,acceleration,accel_vert,velocity,yaw,pitch,roll\n";
#endif

static void session_close(void)
{
	if (!atomic_get(&stats.open)) {
		return;
	}

	(void)fs_close(&bin_file);
#if defined(CONFIG_TELEMETRY_LOGGER_CSV)
	(void)fs_close(&csv_file);
#endif
	atomic_set(&stats.open, 0);
}

static int session_open(void)
{
	uint32_t index;
	int rc;

	rc = mount_card();
	if (rc) {
		return rc;
	}

	rc = ensure_dir();
	if (rc) {
		LOG_ERR("cannot create %s (%d)", CONFIG_TELEMETRY_LOGGER_DIR, rc);
		return rc;
	}

	rc = next_index(&index);
	if (rc) {
		LOG_ERR("cannot list %s (%d)", CONFIG_TELEMETRY_LOGGER_DIR, rc);
		return rc;
	}

	rc = open_file(&bin_file, index, "BIN");
	if (rc) {
		return rc;
	}

#if defined(CONFIG_TELEMETRY_LOGGER_CSV)
	rc = open_file(&csv_file, index, "CSV");
	if (rc) {
		(void)fs_close(&bin_file);
		return rc;
	}

	ssize_t n = fs_write(&csv_file, csv_header, sizeof(csv_header) - 1);

	if (n != (ssize_t)(sizeof(csv_header) - 1)) {
		(void)fs_close(&bin_file);
		(void)fs_close(&csv_file);
		return (n < 0) ? (int)n : -EIO;
	}
#endif

	bin_bytes = 0;
	atomic_set(&stats.index, (atomic_val_t)index);
	atomic_inc(&stats.sessions);
	atomic_set(&stats.open, 1);

	LOG_INF("recording to %s/" FILE_PREFIX "%04u.BIN",
		CONFIG_TELEMETRY_LOGGER_DIR, index);
	return 0;
}

/* ---- writing ------------------------------------------------------------ */

#if defined(CONFIG_TELEMETRY_LOGGER_CSV)
/* Decode only frames whose length matches the layout exactly: a payload
 * that is longer or shorter than its type implies came from a firmware
 * with a different wire revision, and decoding it by offset would print
 * numbers that look plausible and are wrong. It still gets a row, with
 * only the framing filled in, and is in the .BIN in full.
 */
static int format_row(const struct rx_frame *f, char *buf, size_t buf_sz)
{
	const uint8_t type = f->data[2];
	const uint8_t len = f->data[3];
	const uint8_t *payload = &f->data[AURORA_TELEMETRY_WIRE_HDR_LEN];

	if (type == AURORA_TELEMETRY_WIRE_TYPE_SM_UPDATE &&
	    len == sizeof(struct telemetry_wire_sm_update)) {
		struct telemetry_wire_sm_update u;

		memcpy(&u, payload, sizeof(u));
		return snprintf(buf, buf_sz,
				"%lld,%u,%u,%u,%u,%u,%u,,"
				"%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f\n",
				(long long)f->rx_ms, type, len,
				u.timestamp_ms, u.state, u.armed, u.sm_type,
				u.altitude, u.acceleration, u.accel_vert,
				u.velocity, u.orientation[0],
				u.orientation[1], u.orientation[2]);
	}

	if (type == AURORA_TELEMETRY_WIRE_TYPE_STATUS &&
	    len == sizeof(struct telemetry_wire_status)) {
		struct telemetry_wire_status s;

		memcpy(&s, payload, sizeof(s));
		return snprintf(buf, buf_sz,
				"%lld,%u,%u,%u,%u,%u,%u,0x%02x,,,,,,,\n",
				(long long)f->rx_ms, type, len,
				s.timestamp_ms, s.state,
				(s.flags & AURORA_TELEMETRY_WIRE_STATUS_ARMED) ? 1 : 0,
				s.sm_type, s.flags);
	}

	return snprintf(buf, buf_sz, "%lld,%u,%u,,,,,,,,,,,,\n",
			(long long)f->rx_ms, type, len);
}
#endif /* CONFIG_TELEMETRY_LOGGER_CSV */

static int write_all(struct fs_file_t *file, const void *data, size_t len)
{
	ssize_t n = fs_write(file, data, len);

	if (n < 0) {
		return (int)n;
	}
	return (n == (ssize_t)len) ? 0 : -ENOSPC;
}

static int record(const struct rx_frame *f)
{
	int rc = write_all(&bin_file, f->data, f->len);

	if (rc) {
		return rc;
	}
	bin_bytes += f->len;

#if defined(CONFIG_TELEMETRY_LOGGER_CSV)
	char row[192];
	int n = format_row(f, row, sizeof(row));

	if (n > 0) {
		rc = write_all(&csv_file, row, MIN((size_t)n, sizeof(row) - 1));
		if (rc) {
			return rc;
		}
	}
#endif

	disk_led_activity();
	return 0;
}

static int sync_files(void)
{
	int rc = fs_sync(&bin_file);

#if defined(CONFIG_TELEMETRY_LOGGER_CSV)
	int rc_csv = fs_sync(&csv_file);

	if (rc == 0) {
		rc = rc_csv;
	}
#endif
	return rc;
}

/* Drop the recording after a failed write or sync. The next attempt opens
 * a fresh index on a re-probed card instead of appending to a file whose
 * tail is in an unknown state.
 */
static void write_failed(int rc)
{
	atomic_inc(&stats.write_errs);
	LOG_ERR("write to " FILE_PREFIX "%04u failed (%d), reopening",
		(uint32_t)atomic_get(&stats.index), rc);
	session_close();
	unmount_card();
}

static void recorder_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct rx_frame f;
	int64_t next_open = 0;
	int64_t last_sync = 0;
	bool dirty = false;
	bool warned = false;

	while (1) {
		int rc = k_msgq_get(&frame_q, &f,
				    K_MSEC(CONFIG_TELEMETRY_LOGGER_SYNC_INTERVAL_MS));
		int64_t now = k_uptime_get();

		if (!atomic_get(&stats.open) && now >= next_open) {
			int orc = session_open();

			if (orc == 0) {
				warned = false;
				last_sync = now;
			} else {
				unmount_card();
				if (!warned) {
					LOG_WRN("no recording open (%d), retrying "
						"every %d ms", orc,
						CONFIG_TELEMETRY_LOGGER_RETRY_MS);
					warned = true;
				}
				next_open = now + CONFIG_TELEMETRY_LOGGER_RETRY_MS;
			}
		}

		if (rc == 0) {
			if (!atomic_get(&stats.open)) {
				atomic_inc(&stats.unrecorded);
			} else if ((rc = record(&f)) != 0) {
				write_failed(rc);
				next_open = now;
				dirty = false;
			} else {
				atomic_inc(&stats.recorded);
				dirty = true;
			}
		}

		if (dirty && now - last_sync >=
				     CONFIG_TELEMETRY_LOGGER_SYNC_INTERVAL_MS) {
			rc = sync_files();
			if (rc) {
				write_failed(rc);
				next_open = now;
			}
			dirty = false;
			last_sync = now;
		}
	}
}

K_THREAD_DEFINE(tlog_recorder_tid, CONFIG_TELEMETRY_LOGGER_WRITER_STACK_SIZE,
		recorder_thread, NULL, NULL, NULL,
		CONFIG_TELEMETRY_LOGGER_WRITER_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

int recorder_init(void)
{
	disk_led_init();
	k_thread_start(tlog_recorder_tid);
	return 0;
}

void recorder_get_stats(struct recorder_stats *out)
{
	if (!out) {
		return;
	}
	out->recorded = (uint32_t)atomic_get(&stats.recorded);
	out->dropped = (uint32_t)atomic_get(&stats.dropped);
	out->unrecorded = (uint32_t)atomic_get(&stats.unrecorded);
	out->write_errs = (uint32_t)atomic_get(&stats.write_errs);
	out->sessions = (uint32_t)atomic_get(&stats.sessions);
	out->open = atomic_get(&stats.open) != 0;
	out->index = (uint32_t)atomic_get(&stats.index);
	out->bytes = bin_bytes;
}
