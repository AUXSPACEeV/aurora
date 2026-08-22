/**
 * @file ground_state.c
 * @brief Mutex-protected ground-station snapshot (see ground_state.h).
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ground_state.h"

#include <string.h>
#include <zephyr/kernel.h>

/* The critical sections are a handful of word copies, so a mutex is plenty:
 * no producer holds it long enough to matter to another, and the display is
 * the only reader.
 */
static K_MUTEX_DEFINE(gs_lock);
static struct gs_snapshot gs_state;

void gs_init(void)
{
	k_mutex_lock(&gs_lock, K_FOREVER);
	memset(&gs_state, 0, sizeof(gs_state));
	k_mutex_unlock(&gs_lock);
}

void gs_set_local(const struct gs_local *local)
{
	k_mutex_lock(&gs_lock, K_FOREVER);
	gs_state.local = *local;
	k_mutex_unlock(&gs_lock);
}

void gs_set_gnss(const struct gs_gnss *gnss)
{
	k_mutex_lock(&gs_lock, K_FOREVER);
	gs_state.gnss = *gnss;
	k_mutex_unlock(&gs_lock);
}

void gs_set_telemetry(const struct gs_telemetry *tlm)
{
	k_mutex_lock(&gs_lock, K_FOREVER);
	gs_state.tlm = *tlm;
	k_mutex_unlock(&gs_lock);
}

void gs_get(struct gs_snapshot *out)
{
	k_mutex_lock(&gs_lock, K_FOREVER);
	*out = gs_state;
	k_mutex_unlock(&gs_lock);
}
