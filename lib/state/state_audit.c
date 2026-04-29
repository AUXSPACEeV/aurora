/**
 * @file state_audit.c
 * @brief Ring-buffer audit log for state machine transitions and events.
 *
 * Producers (the state machine) call sm_audit_transition / sm_audit_event
 * from the SM hot path. Each call updates the in-RAM ring synchronously
 * (so the shell sees fresh entries immediately) and then hands a copy to
 * an internal k_msgq with K_NO_WAIT. A dedicated writer thread drains
 * the queue and performs all filesystem I/O. Keeping FS calls off the SM
 * thread avoids ever blocking a flight-critical task on SD-card latency.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>

#include <aurora/lib/state/audit.h>

LOG_MODULE_REGISTER(state_audit, CONFIG_STATE_MACHINE_LOG_LEVEL);

#define AUDIT_SIZE       CONFIG_AURORA_STATE_MACHINE_AUDIT_LOG_SIZE
#define AUDIT_MSGQ_DEPTH AUDIT_SIZE
#define AUDIT_WRITER_STACK 2048
#define AUDIT_WRITER_PRIO  10
#define MAX_F_RETRIES 3

/* Ring buffer state (queryable from the shell). All access is serialised
 * by ring_mutex — producers (sm_audit_transition / sm_audit_event) and
 * shell readers (sm_audit_get / count / clear) all hold it briefly. The
 * critical section never makes blocking calls, so the SM thread can take
 * it without back-pressure from the FS.
 */
static struct sm_audit_entry ring[AUDIT_SIZE];
static uint32_t head;
static uint32_t count;
static K_MUTEX_DEFINE(ring_mutex);

/* File state — only touched by the writer thread, no locking needed. */
static struct fs_file_t audit_file;
static int audit_file_exists;
static int audit_file_retry_cnt;

K_MSGQ_DEFINE(audit_msgq, sizeof(struct sm_audit_entry),
	      AUDIT_MSGQ_DEPTH, 4);

static int sm_audit_file_write_header(void)
{
	char header[53];
	ssize_t wr;

	snprintf(header, sizeof(header),
		 "%-12.12s %-12.12s %-12.12s %-12.12s\n",
		 "Time (ms)", "Type", "From", "To / Event");

	wr = fs_write(&audit_file, header, strlen(header));
	wr += fs_write(&audit_file,
		       "---------------------------------------------------\n",
		       50);
	if (wr < 0) {
		LOG_ERR("Could not write header to state machine audit file.");
		return (int)wr;
	}
	LOG_DBG("State machine audit file header done.");

	return 0;
}

static int sm_audit_file_create(void)
{
	struct fs_dir_t ptr;
	struct fs_dirent entry;
	char dir[64];
	char full_path[64];
	char *sep;
	int rc;

	/* Build "<base_path>/audit.<i>" */
	for (int i = 0; i <= CONFIG_AURORA_STATE_MACHINE_AUDIT_MAX_FILES; i++) {
		rc = snprintf(full_path, sizeof(full_path), "%s/audit.%d",
			      CONFIG_AURORA_STATE_MACHINE_AUDIT_BASE_PATH, i);

		if (rc < 0 || rc >= (int)sizeof(full_path))
			return -ENAMETOOLONG;

		if (fs_stat(full_path, &entry) == -ENOENT)
			break;

		/* All files already exist */
		if (i == CONFIG_AURORA_STATE_MACHINE_AUDIT_MAX_FILES) {
			rc = snprintf(full_path, sizeof(full_path), "%s/audit.0",
					CONFIG_AURORA_STATE_MACHINE_AUDIT_BASE_PATH);

			if (rc < 0 || rc >= (int)sizeof(full_path))
				return -ENAMETOOLONG;

			LOG_WRN("Could not get new file for audit data. Falling back to %s", full_path);
		}
	}

	/* Ensure parent directory exists (fs_open creates files, not dirs). */
	strncpy(dir, full_path, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	sep = strrchr(dir, '/');
	if (sep != NULL && sep != dir) {
		*sep = '\0';
		fs_dir_t_init(&ptr);
		if (fs_opendir(&ptr, dir) < 0) {
			(void)fs_mkdir(dir);
		} else {
			fs_closedir(&ptr);
		}
	}

	fs_file_t_init(&audit_file);
	rc = fs_open(&audit_file, full_path,
		     FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc != 0) {
		LOG_ERR("failed to open %s (%d)", full_path, rc);
		return rc;
	}

	audit_file_exists = 1;
	LOG_INF("State machine audit file created: %s", full_path);

	return 0;
}

static int write_entry(const struct sm_audit_entry *e)
{
	char buf[128];
	ssize_t wr;

	if (e == NULL)
		return -EINVAL;

	if (!audit_file_exists)
		return -ENOENT;

	if (e->type == SM_AUDIT_TRANSITION) {
		snprintf(buf, sizeof(buf), "%-12llu %-12s %-12s %s",
			(unsigned long long)e->timestamp_ns,
			"transition",
			sm_state_str(e->from),
			sm_state_str(e->to));
	} else {
		snprintf(buf, sizeof(buf), "%-12llu %-12s %-12s %s",
			(unsigned long long)e->timestamp_ns,
			"event",
			sm_state_str(e->from),
			e->event ? e->event : "");
	}

	wr = fs_write(&audit_file, buf, strlen(buf));
	if (wr < 0)
		return (int)wr;

	wr = fs_write(&audit_file, "\n", 1);
	if (wr < 0)
		return (int)wr;

	LOG_INF("%s", buf);

	return fs_sync(&audit_file);
}

static void ring_push(const struct sm_audit_entry *e)
{
	k_mutex_lock(&ring_mutex, K_FOREVER);
	ring[head] = *e;
	head = (head + 1) % AUDIT_SIZE;
	if (count < AUDIT_SIZE) {
		count++;
	}
	k_mutex_unlock(&ring_mutex);
}

static void audit_writer_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct sm_audit_entry e;
	int rc;

	while (1) {
		if (k_msgq_get(&audit_msgq, &e, K_FOREVER) != 0) {
			continue;
		}

		if (!audit_file_exists &&
		    audit_file_retry_cnt++ < MAX_F_RETRIES) {
			rc = sm_audit_file_create();
			if (rc) {
				LOG_ERR("Could not create audit file (%d)", rc);
			} else {
				rc = sm_audit_file_write_header();
				if (rc) {
					LOG_ERR("Could not create header (%d)", rc);
				}
			}
		}

		rc = write_entry(&e);
		if (rc && rc != -ENOENT) {
			LOG_ERR("Failed to write to audit file (%d)", rc);
		}
	}
}

K_THREAD_DEFINE(audit_writer_th, AUDIT_WRITER_STACK,
		audit_writer_task, NULL, NULL, NULL,
		AUDIT_WRITER_PRIO, 0, 0);

static void publish(const struct sm_audit_entry *e)
{
	/* Update the in-RAM ring synchronously so the shell sees the entry
	 * immediately, then hand a copy to the writer thread for FS I/O.
	 * Drop on queue overflow rather than block the SM hot path; the
	 * ring still reflects the entry for live diagnostics.
	 */
	ring_push(e);

	if (k_msgq_put(&audit_msgq, e, K_NO_WAIT) != 0) {
		LOG_WRN("audit queue full, entry not persisted");
	}
}

void sm_audit_transition(enum sm_state from, enum sm_state to)
{
	struct sm_audit_entry e = {
		.timestamp_ns = k_ticks_to_ns_floor64(k_uptime_ticks()),
		.type = SM_AUDIT_TRANSITION,
		.from = from,
		.to = to,
		.event = NULL,
	};

	publish(&e);
}

void sm_audit_event(enum sm_state state, const char *event)
{
	struct sm_audit_entry e = {
		.timestamp_ns = k_ticks_to_ns_floor64(k_uptime_ticks()),
		.type = SM_AUDIT_EVENT,
		.from = state,
		.to = state,
		.event = event,
	};

	publish(&e);
}

uint32_t sm_audit_count(void)
{
	uint32_t c;

	k_mutex_lock(&ring_mutex, K_FOREVER);
	c = count;
	k_mutex_unlock(&ring_mutex);

	return c;
}

int sm_audit_get(uint32_t idx, struct sm_audit_entry *entry)
{
	uint32_t pos;

	if (entry == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&ring_mutex, K_FOREVER);

	if (idx >= count) {
		k_mutex_unlock(&ring_mutex);
		return -EINVAL;
	}

	/* oldest entry is at (head - count) mod AUDIT_SIZE */
	pos = (head + AUDIT_SIZE - count + idx) % AUDIT_SIZE;
	*entry = ring[pos];

	k_mutex_unlock(&ring_mutex);

	return 0;
}

void sm_audit_clear(void)
{
	k_mutex_lock(&ring_mutex, K_FOREVER);
	head = 0;
	count = 0;
	k_mutex_unlock(&ring_mutex);
}
