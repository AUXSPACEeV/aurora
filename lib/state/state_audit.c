/**
 * @file state_audit.c
 * @brief Ring-buffer audit log for state machine transitions and events.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#include <aurora/lib/state/audit.h>

#define AUDIT_SIZE CONFIG_AURORA_STATE_MACHINE_AUDIT_LOG_SIZE

static struct sm_audit_entry ring[AUDIT_SIZE];
static uint32_t head;	/* next write position */
static uint32_t count;	/* entries stored      */
static struct k_spinlock lock;

static void append(const struct sm_audit_entry *e)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ring[head] = *e;
	head = (head + 1) % AUDIT_SIZE;
	if (count < AUDIT_SIZE) {
		count++;
	}

	k_spin_unlock(&lock, key);
}

void sm_audit_transition(enum sm_state from, enum sm_state to)
{
	struct sm_audit_entry e = {
		.timestamp_ms = k_uptime_get(),
		.type = SM_AUDIT_TRANSITION,
		.from = from,
		.to = to,
		.event = NULL,
	};

	append(&e);
}

void sm_audit_event(enum sm_state state, const char *event)
{
	struct sm_audit_entry e = {
		.timestamp_ms = k_uptime_get(),
		.type = SM_AUDIT_EVENT,
		.from = state,
		.to = state,
		.event = event,
	};

	append(&e);
}

uint32_t sm_audit_count(void)
{
	return count;
}

int sm_audit_get(uint32_t idx, struct sm_audit_entry *entry)
{
	k_spinlock_key_t key;
	uint32_t pos;

	if (entry == NULL || idx >= count) {
		return -EINVAL;
	}

	key = k_spin_lock(&lock);

	/* oldest entry is at (head - count) mod AUDIT_SIZE */
	pos = (head + AUDIT_SIZE - count + idx) % AUDIT_SIZE;
	*entry = ring[pos];

	k_spin_unlock(&lock, key);

	return 0;
}

void sm_audit_clear(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	head = 0;
	count = 0;

	k_spin_unlock(&lock, key);
}
