/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file state_retain.c
 * @brief Flight state retention across a watchdog reset.
 *
 * See retain.h for the contract.  The storage choice is the interesting
 * part: the record sits in RTC slow memory rather than flash.
 *
 * Flash would survive a power cycle, but the state machine writes on every
 * transition, and the transitions that matter most (APOGEE, MAIN, REDUNDANT)
 * are the ones that must not stall.
 * A flash page erase on the ESP32S3 SoC blocks for milliseconds.
 *
 * RTC slow memory writes are a few stores, there is no wear, and the segment is
 * 8 KiB of which this uses a couple of dozen bytes.
 * It buys less than flash and it does not survive losing the supply.
 * But it covers being recovered from a watchdog-driven warm reset.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include <zephyr/drivers/hwinfo.h>

#include <aurora/lib/state/retain.h>

LOG_MODULE_DECLARE(state_machine, CONFIG_STATE_MACHINE_LOG_LEVEL);

#define SM_RETAIN_MAGIC   0xA5F1D307u
#define SM_RETAIN_VERSION 2u

/**
 * @brief Retained flight record.
 *
 * @c crc covers every byte after itself, so the field order matters: keep
 * @c crc last in the header and all payload behind it.
 */
struct sm_retain_record {
	uint32_t magic;
	uint16_t version;
	uint16_t size;   /**< sizeof(record), rejects a silent layout change. */
	uint32_t crc;    /**< CRC-32/IEEE over everything after this field.   */

	/* --- payload --- */
	uint8_t  state;     /**< enum sm_state at the last transition. */
	uint8_t  sm_type;   /**< enum sm_type that wrote it. */
	uint16_t recoveries;/**< Watchdog recoveries since last power cycle. */

	/* --- Opaque payload --- */
	uint16_t blob_len;
	uint8_t  blob[SM_RETAIN_BLOB_SIZE];
};

/* Deliberately uninitialised: .rtc_noinit is NOLOAD and the startup code
 * does not clear it, which is the whole point.
 * Giving this an initialiser would place it in a loaded section and zero it on
 * every boot.
 */
static struct sm_retain_record retain_rec
	__attribute__((section(".rtc_noinit")));

static uint32_t boot_reset_cause;
static bool boot_reset_cause_valid;
static bool recovered_this_boot;

static uint32_t record_crc(const struct sm_retain_record *rec)
{
	const uint8_t *payload = (const uint8_t *)rec + offsetof(struct sm_retain_record, crc) +
				 sizeof(rec->crc);
	const size_t len = sizeof(*rec) - offsetof(struct sm_retain_record, crc) -
			   sizeof(rec->crc);

	return crc32_ieee(payload, len);
}

static bool record_valid(const struct sm_retain_record *rec)
{
	return rec->magic == SM_RETAIN_MAGIC &&
	       rec->version == SM_RETAIN_VERSION &&
	       rec->size == sizeof(*rec) &&
	       rec->crc == record_crc(rec);
}

static int sm_retain_init(void)
{
	int rc = hwinfo_get_reset_cause(&boot_reset_cause);

	boot_reset_cause_valid = (rc == 0);
	if (rc != 0) {
		LOG_WRN("reset cause unavailable (%d), "
			"flight recovery disabled", rc);
	}

	if (!boot_reset_cause_valid ||
	   (boot_reset_cause & RESET_WATCHDOG) == 0u) {
		sm_retain_invalidate();
	}

	return 0;
}

SYS_INIT(sm_retain_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* sm_retain_save - see retain.h */
void sm_retain_save(enum sm_state state)
{
	if (state == SM_ERROR) {
		/* SM_ERROR is a latched fault, not a phase of flight, and the
		 * core saves on every transition including this one.  Resuming
		 * into it puts the board straight back into the error it just
		 * reset out of -- observed as a boot that goes IDLE -> ERROR
		 * before any input has been read, needing a disarm to clear.
		 *
		 * Leaving the previous state in the record is also the more
		 * useful behaviour: a pre-flight error leaves SM_IDLE there,
		 * which restore already refuses, and an in-flight abort
		 * recovers to IDLE through the error handler and saves that.
		 */
		return;
	}

	retain_rec.magic = SM_RETAIN_MAGIC;
	retain_rec.version = SM_RETAIN_VERSION;
	retain_rec.size = sizeof(retain_rec);
	retain_rec.state = (uint8_t)state;
	retain_rec.sm_type = (uint8_t)sm_get_type();
	retain_rec.crc = record_crc(&retain_rec);
}

/* sm_retain_invalidate - see retain.h */
void sm_retain_invalidate(void)
{
	memset(&retain_rec, 0, sizeof(retain_rec));
	recovered_this_boot = false;
}

/* sm_retain_recovery_count - see retain.h */
uint16_t sm_retain_recovery_count(void)
{
	return record_valid(&retain_rec) ? retain_rec.recoveries : 0u;
}

/* sm_retain_recovered - see retain.h */
bool sm_retain_recovered(void)
{
	return recovered_this_boot;
}

/* sm_retain_save_blob - see retain.h */
int sm_retain_save_blob(const void *data, size_t len)
{
	if (data == NULL || len > sizeof(retain_rec.blob)) {
		return -EINVAL;
	}

	memcpy(retain_rec.blob, data, len);
	retain_rec.blob_len = (uint16_t)len;

	/* The header may not have been written yet if no transition has
	 * happened, so build it here too rather than leaving a payload behind
	 * an invalid magic.
	 */
	retain_rec.magic = SM_RETAIN_MAGIC;
	retain_rec.version = SM_RETAIN_VERSION;
	retain_rec.size = sizeof(retain_rec);
	retain_rec.crc = record_crc(&retain_rec);

	return 0;
}

/* sm_retain_load_blob - see retain.h */
int sm_retain_load_blob(void *data, size_t len)
{
	if (data == NULL) {
		return -EINVAL;
	}

	if (!record_valid(&retain_rec) || retain_rec.blob_len == 0u) {
		return -ENOENT;
	}

	if (retain_rec.blob_len != (uint16_t)len) {
		LOG_WRN("retained payload is %u B, "
			"caller wants %u B; ignoring",
			(unsigned int)retain_rec.blob_len,
			(unsigned int)len);
		return -EINVAL;
	}

	memcpy(data, retain_rec.blob, len);

	return 0;
}

/* sm_retain_restore - see retain.h */
int sm_retain_restore(enum sm_state *out)
{
	__ASSERT(out != NULL, "sm_retain_restore: destination is NULL.");

	if (!boot_reset_cause_valid || (boot_reset_cause & RESET_WATCHDOG) == 0u) {
		sm_retain_invalidate();
		return -ENOTSUP;
	}

	if (!record_valid(&retain_rec)) {
		sm_retain_invalidate();
		return -ENOENT;
	}

	if (retain_rec.sm_type != (uint8_t)sm_get_type()) {
		LOG_WRN("retained state came from backend %u, running %u; ignoring",
			retain_rec.sm_type, (unsigned int)sm_get_type());
		sm_retain_invalidate();
		return -EINVAL;
	}

	/* SM_IDLE means nothing was in progress; SM_ERROR is a latched fault
	 * that must not be re-entered on boot.  sm_retain_save() already
	 * declines to persist SM_ERROR -- this also covers a record written
	 * by an older build that did.
	 */
	if ((enum sm_state)retain_rec.state == SM_IDLE ||
	    (enum sm_state)retain_rec.state == SM_ERROR) {
		return -EINVAL;
	}

	retain_rec.recoveries++;
	retain_rec.crc = record_crc(&retain_rec);

	*out = (enum sm_state)retain_rec.state;
	recovered_this_boot = true;

	return 0;
}
