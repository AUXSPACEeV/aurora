/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/wdt_recovery.h>
#include <aurora/lib/watchdog.h>
#if defined(CONFIG_BARO)
#include <aurora/lib/baro.h>
#endif /* CONFIG_BARO */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/retention/retention.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wdt_recovery, CONFIG_AURORA_WDT_RECOVERY_LOG_LEVEL);

#define RETENTION_NODE DT_CHOSEN(auxspace_recovery)
BUILD_ASSERT(DT_NODE_HAS_STATUS(RETENTION_NODE, okay),
	     "chosen auxspace,recovery is missing or disabled");

/** @brief Bump when the on-storage layout changes. */
#define RECORD_VERSION 2

/** @brief Set by the watchdog supervisor immediately before it resets the SoC. */
#define RECORD_FLAG_WDT_PENDING BIT(0)

/**
 * @brief Retained record layout.
 *
 * Kept intentionally tiny; the retention subsystem adds its own magic prefix
 * and checksum around it (see the devicetree node).
 */
struct wdt_recovery_record {
	uint8_t version;
	uint8_t state; /**< enum sm_state */
	uint8_t flags; /**< RECORD_FLAG_* */
	uint8_t reserved;
	double baro_ref_kpa; /**< Ground-reference pressure to keep altitude framed; 0 = none. */
} __packed;

static const struct device *const ret_dev = DEVICE_DT_GET(RETENTION_NODE);

/* Resolved once by aurora_wdt_recovery_init(). */
static enum aurora_reboot_reason boot_reason = AURORA_REBOOT_UNKNOWN;
static bool recovery_pending;
static enum sm_state recovery_state = SM_IDLE;

/* Sticky within a boot session: once the supervisor declares a reset imminent,
 * every subsequent save keeps the pending marker so a still-running thread
 * cannot clear it before the reset lands (matters on the RP2040, whose hwinfo
 * has no watchdog cause to fall back on).
 */
static atomic_t wdt_pending;

static enum aurora_reboot_reason map_reset_cause(uint32_t cause)
{
	if (cause & RESET_WATCHDOG) {
		return AURORA_REBOOT_WATCHDOG;
	}
	if (cause & RESET_SOFTWARE) {
		return AURORA_REBOOT_SOFTWARE;
	}
	if (cause & RESET_DEBUG) {
		return AURORA_REBOOT_DEBUG;
	}
	if (cause & RESET_PIN) {
		return AURORA_REBOOT_PIN;
	}
	if (cause & (RESET_POR | RESET_BROWNOUT)) {
		return AURORA_REBOOT_POWER_ON;
	}
	return AURORA_REBOOT_UNKNOWN;
}

static bool record_read(struct wdt_recovery_record *out)
{
	if (retention_is_valid(ret_dev) != 1) {
		return false;
	}
	if (retention_read(ret_dev, 0, (uint8_t *)out, sizeof(*out)) != 0) {
		return false;
	}
	return out->version == RECORD_VERSION;
}

int aurora_wdt_recovery_init(void)
{
	uint32_t cause = 0;
	bool hw_wdt = false;

	if (!device_is_ready(ret_dev)) {
		LOG_ERR("retention device %s not ready; recovery disabled",
			ret_dev->name);
		return 0;
	}

	/* Hardware reset cause. Not every SoC reports a watchdog bit (the
	 * RP2040 does not), so it is only one of the two watchdog signals.
	 */
	if (hwinfo_get_reset_cause(&cause) == 0) {
		boot_reason = map_reset_cause(cause);
		hw_wdt = (cause & RESET_WATCHDOG) != 0;
		/* Latch is sticky across resets; clear so the next boot starts
		 * from a clean slate. Ignored if the driver lacks clear support.
		 */
		(void)hwinfo_clear_reset_cause();
	}

	struct wdt_recovery_record rec;
	bool have_record = record_read(&rec);
	bool sw_wdt = have_record && (rec.flags & RECORD_FLAG_WDT_PENDING);

	if (hw_wdt || sw_wdt) {
		boot_reason = AURORA_REBOOT_WATCHDOG;
	}

	/* Recover only from a watchdog reset with a state worth resuming. */
	if (boot_reason == AURORA_REBOOT_WATCHDOG && have_record &&
	    rec.state != SM_IDLE && rec.state != SM_ERROR) {
		recovery_pending = true;
		recovery_state = (enum sm_state)rec.state;

#if defined(CONFIG_BARO)
		/* Pin the altitude frame to the pre-reset reference before the
		 * baro thread takes its first post-reboot sample. Only the first
		 * baro_set_reference() wins, so this must run early (it does:
		 * called from main() before the sensor threads reach altitude
		 * conversion). Keeps the SM altitude thresholds meaningful.
		 */
		if (rec.baro_ref_kpa > 0.0) {
			(void)baro_set_reference(rec.baro_ref_kpa);
		}
#endif /* CONFIG_BARO */
	}

	/* Consume the pending marker so an unrelated later reset does not
	 * recover again. The state is left in place and refreshed on the next
	 * aurora_wdt_recovery_save_state().
	 */
	if (have_record && (rec.flags & RECORD_FLAG_WDT_PENDING)) {
		rec.flags &= ~RECORD_FLAG_WDT_PENDING;
		(void)retention_write(ret_dev, 0, (const uint8_t *)&rec, sizeof(rec));
	}

	if (recovery_pending) {
		LOG_WRN("watchdog reset detected; will recover flight state %d",
			recovery_state);
	} else {
		LOG_INF("boot reason %d; no recovery pending", boot_reason);
	}
	return 0;
}

enum aurora_reboot_reason aurora_wdt_recovery_reason(void)
{
	return boot_reason;
}

bool aurora_wdt_recovery_pending(enum sm_state *state_out)
{
	if (recovery_pending && state_out != NULL) {
		*state_out = recovery_state;
	}
	return recovery_pending;
}

void aurora_wdt_recovery_save_state(enum sm_state state)
{
	struct wdt_recovery_record rec = {
		.version = RECORD_VERSION,
		.state = (uint8_t)state,
		.flags = atomic_get(&wdt_pending) ? RECORD_FLAG_WDT_PENDING : 0,
	};

#if defined(CONFIG_BARO)
	/* Constant once the first post-boot sample sets it; persist it so a
	 * recovery keeps altitude in the same frame as before the reset.
	 */
	rec.baro_ref_kpa = baro_get_reference();
#endif /* CONFIG_BARO */

	if (!device_is_ready(ret_dev)) {
		return;
	}
	(void)retention_write(ret_dev, 0, (const uint8_t *)&rec, sizeof(rec));
}

/*
 * Strong override of the watchdog supervisor's weak hook. Runs in the monitor
 * thread the moment it decides to let the hardware watchdog reset the SoC, so
 * we can stamp the record as a watchdog reset. This is what lets the RP2040
 * (whose hwinfo cannot report a watchdog cause) still be recovered.
 */
void aurora_wdt_reset_imminent(void)
{
	struct wdt_recovery_record rec;

	atomic_set(&wdt_pending, 1);

	if (!device_is_ready(ret_dev)) {
		return;
	}
	/* Preserve the last saved state; only raise the pending flag. If no
	 * record exists yet there is nothing mid-flight to recover.
	 */
	if (!record_read(&rec)) {
		return;
	}
	rec.flags |= RECORD_FLAG_WDT_PENDING;
	(void)retention_write(ret_dev, 0, (const uint8_t *)&rec, sizeof(rec));
}
