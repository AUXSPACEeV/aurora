/**
 * @file state_config.c
 * @brief Persistent flight thresholds.
 *
 * Different vehicles need different thresholds - a 200 m sport model and a
 * 1 km bird disagree on every altitude and timeout in the set - so the
 * thresholds are runtime data rather than a firmware constant.  The Kconfig
 * values are only the factory defaults.
 *
 * The record lives in a single flash erase page selected by the
 * 'auxspace,sm-config' chosen node.  One page means every save is an
 * erase-then-write, which is fine for a value an operator changes on the
 * bench, not in flight.  A magic, a layout version and a CRC guard against
 * a blank page, a firmware update that reshuffled struct sm_thresholds and
 * a write cut short by a power loss; any of those falls back to the
 * defaults instead of flying a half-written threshold set.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <aurora/lib/state/config.h>

LOG_MODULE_DECLARE(state_machine, CONFIG_STATE_MACHINE_LOG_LEVEL);

#if defined(CONFIG_AURORA_STATE_MACHINE_CONFIG_STORE) && DT_HAS_CHOSEN(auxspace_sm_config)
#define SM_CFG_STORE 1

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#define SM_CFG_NODE   DT_CHOSEN(auxspace_sm_config)
#define SM_CFG_DEV    PARTITION_NODE_DEVICE(SM_CFG_NODE)
#define SM_CFG_OFFSET PARTITION_NODE_OFFSET(SM_CFG_NODE)
#define SM_CFG_SIZE   PARTITION_NODE_SIZE(SM_CFG_NODE)
#endif /* store configured */

/** Marks a written record; also catches an erased (all-0xFF) page. */
#define SM_CFG_MAGIC 0x41535443U /* "ASTC" */

/* Bump whenever struct sm_thresholds changes shape, so an older record is
 * rejected rather than reinterpreted field by field.
 */
#define SM_CFG_VERSION 1U

/** @brief On-flash layout.  Appended to, never reordered; bump the version. */
struct sm_cfg_record {
	uint32_t magic;
	uint32_t version;
	uint32_t size;   /**< sizeof(struct sm_thresholds) when written. */
	uint32_t crc;    /**< CRC-32 IEEE over the thresholds below. */
	struct sm_thresholds th;
};

/*-----------------------------------------------------------
 * Defaults and field table
 *----------------------------------------------------------*/

/* sm_config_defaults – see config.h */
void sm_config_defaults(struct sm_thresholds *out)
{
	__ASSERT(out != NULL, "sm_config_defaults: destination is NULL.");

	*out = (struct sm_thresholds){
		/* Sensor Metrics */
		.T_AB = CONFIG_BOOST_ACCELERATION,
		.T_H = CONFIG_BOOST_ALTITUDE,
		.T_BB = CONFIG_BURNOUT_ACCELERATION,
		.T_M = CONFIG_MAIN_DESCENT_HEIGHT,
		.T_L = CONFIG_LANDING_VELOCITY,
		.T_OA = CONFIG_ARM_ANGLE,
		.T_OI = CONFIG_DISARM_ANGLE,
		.N_OI = CONFIG_DISARM_ANGLE_SAMPLES,

		/* Timers */
		.DT_AB = CONFIG_BOOST_TIMER_MS,
		.DT_L = CONFIG_LANDING_TIMER_MS,

		/* Timeouts */
		.TO_A = CONFIG_APOGEE_TIMEOUT_MS,
		.TO_M = CONFIG_MAIN_TIMEOUT_MS,
		.TO_R = CONFIG_REDUNDANT_TIMEOUT_MS,
	};
}

/* Mirrors the simple backend's struct sm_thresholds; a second backend would
 * bring its own table alongside this one.
 */
#define FIELD(f, u, mn, mx, d) \
	{ #f, u, d, offsetof(struct sm_thresholds, f), mn, mx }

static const struct sm_config_field fields[] = {
	FIELD(T_AB, "m/s^2", 0, 1000, "ARMED->BOOST acceleration"),
	FIELD(T_H, "m", 0, 100000, "ARMED->BOOST altitude"),
	FIELD(T_BB, "m/s^2", 0, 1000, "BOOST->BURNOUT acceleration"),
	FIELD(T_M, "m", 0, 100000, "APOGEE->MAIN descent height"),
	FIELD(T_L, "m/s", 0, 1000, "Landing velocity"),
	FIELD(T_OA, "deg", 0, 90, "IDLE->ARMED elevation"),
	FIELD(T_OI, "deg", 0, 90, "ARMED->IDLE elevation"),
	FIELD(N_OI, "samples", 1, 1000, "Samples below T_OI to disarm"),
	FIELD(DT_AB, "ms", 0, 600000, "T_AB and T_H hold time"),
	FIELD(DT_L, "ms", 0, 600000, "T_L hold time"),
	FIELD(TO_A, "ms", 0, 3600000, "APOGEE timeout"),
	FIELD(TO_M, "ms", 0, 3600000, "MAIN to REDUNDANT delay"),
	FIELD(TO_R, "ms", 0, 3600000, "REDUNDANT timeout"),
};

#undef FIELD

/** @brief Case-insensitive string compare, so "t_m" finds "T_M". */
static bool name_eq(const char *a, const char *b)
{
	__ASSERT(a != NULL, "name_eq: a is NULL.");
	__ASSERT(b != NULL, "name_eq: b is NULL.");

	while (*a && *b) {
		char ca = *a >= 'a' && *a <= 'z' ? *a - ('a' - 'A') : *a;
		char cb = *b >= 'a' && *b <= 'z' ? *b - ('a' - 'A') : *b;

		if (ca != cb) {
			return false;
		}
		a++;
		b++;
	}
	return *a == *b;
}

/* sm_config_fields – see config.h */
const struct sm_config_field *sm_config_fields(size_t *count)
{
	__ASSERT(count != NULL, "sm_config_fields: destination is NULL.");

	*count = ARRAY_SIZE(fields);
	return fields;
}

/* sm_config_field_find – see config.h */
const struct sm_config_field *sm_config_field_find(const char *name)
{
	__ASSERT(name != NULL, "sm_config_field_find: name is NULL.");

	for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
		if (name_eq(fields[i].name, name)) {
			return &fields[i];
		}
	}
	return NULL;
}

/* sm_config_field_get – see config.h */
int sm_config_field_get(const struct sm_thresholds *cfg,
			const struct sm_config_field *field)
{
	__ASSERT(cfg != NULL, "sm_config_field_get: config is NULL.");
	__ASSERT(field != NULL, "sm_config_field_get: field is NULL.");

	int value;

	memcpy(&value, (const uint8_t *)cfg + field->offset, sizeof(value));
	return value;
}

/* sm_config_field_set – see config.h */
int sm_config_field_set(struct sm_thresholds *cfg,
			const struct sm_config_field *field, int value)
{
	if (value < field->min || value > field->max) {
		return -ERANGE;
	}

	memcpy((uint8_t *)cfg + field->offset, &value, sizeof(value));
	return 0;
}

/*-----------------------------------------------------------
 * Flash store
 *----------------------------------------------------------*/

#if defined(SM_CFG_STORE)

/* The whole partition is erased on every save, so it must hold nothing but
 * this record.
 */
BUILD_ASSERT(SM_CFG_SIZE >= sizeof(struct sm_cfg_record),
	     "the auxspace,sm-config partition is too small for the thresholds");

static uint32_t record_crc(const struct sm_thresholds *th)
{
	return crc32_ieee((const uint8_t *)th, sizeof(*th));
}

/* sm_config_load – see config.h */
int sm_config_load(struct sm_thresholds *out)
{
	__ASSERT(out != NULL, "sm_config_load: destination is NULL.");

	const struct device *dev = SM_CFG_DEV;
	struct sm_cfg_record rec;
	int rc;

	sm_config_defaults(out);

	if (!device_is_ready(dev)) {
		LOG_ERR("threshold store %s not ready, using defaults",
			dev->name);
		return -ENODEV;
	}

	rc = flash_read(dev, SM_CFG_OFFSET, &rec, sizeof(rec));
	if (rc) {
		LOG_ERR("threshold read failed (%d), using defaults", rc);
		return rc;
	}

	if (rec.magic != SM_CFG_MAGIC) {
		LOG_INF("no stored thresholds, using defaults");
		return -ENOENT;
	}

	if (rec.version != SM_CFG_VERSION ||
	    rec.size != sizeof(struct sm_thresholds)) {
		LOG_WRN("stored thresholds are v%u/%u B, this build wants "
			"v%u/%u B; using defaults",
			rec.version, rec.size, SM_CFG_VERSION,
			(uint32_t)sizeof(struct sm_thresholds));
		return -ENOENT;
	}

	if (rec.crc != record_crc(&rec.th)) {
		LOG_ERR("stored thresholds are corrupt, using defaults");
		return -ENOENT;
	}

	*out = rec.th;
	LOG_INF("loaded stored flight thresholds");

	return 0;
}

/* sm_config_save – see config.h */
int sm_config_save(const struct sm_thresholds *cfg)
{
	__ASSERT(cfg != NULL, "sm_config_save: config is NULL");

	const struct device *dev = SM_CFG_DEV;
	/* flash_write needs a write-block-size multiple; the record is
	 * int-only, so rounding up to 8 covers every supported flash.
	 */
	static uint8_t buf[ROUND_UP(sizeof(struct sm_cfg_record), 8)];
	struct sm_cfg_record *rec = (struct sm_cfg_record *)buf;
	int rc;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	memset(buf, 0, sizeof(buf));
	rec->magic = SM_CFG_MAGIC;
	rec->version = SM_CFG_VERSION;
	rec->size = sizeof(struct sm_thresholds);
	rec->th = *cfg;
	rec->crc = record_crc(&rec->th);

	rc = flash_erase(dev, SM_CFG_OFFSET, SM_CFG_SIZE);
	if (rc) {
		LOG_ERR("threshold page erase failed (%d)", rc);
		return rc;
	}

	rc = flash_write(dev, SM_CFG_OFFSET, buf, sizeof(buf));
	if (rc) {
		LOG_ERR("threshold write failed (%d)", rc);
		return rc;
	}

	LOG_INF("flight thresholds saved");

	return 0;
}

/* sm_config_erase – see config.h */
int sm_config_erase(void)
{
	const struct device *dev = SM_CFG_DEV;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	return flash_erase(dev, SM_CFG_OFFSET, SM_CFG_SIZE);
}

#else /* no store configured */

/* sm_config_load – see config.h */
int sm_config_load(struct sm_thresholds *out)
{
	__ASSERT(out != NULL, "sm_config_load: destination is NULL.");

	sm_config_defaults(out);
	return -ENOTSUP;
}

/* sm_config_save – see config.h */
int sm_config_save(const struct sm_thresholds *cfg)
{
	ARG_UNUSED(cfg);
	return -ENOTSUP;
}

/* sm_config_erase – see config.h */
int sm_config_erase(void)
{
	return -ENOTSUP;
}

#endif /* SM_CFG_STORE */
