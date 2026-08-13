/**
 * @file main.c
 * @brief Unit tests for the persistent flight threshold store.
 *
 * Runs against the qemu_x86 flash simulator with a 1 KiB partition standing
 * in for the erase page reserved on real hardware.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <aurora/lib/state/config.h>
#include <aurora/lib/state/state.h>

#define SM_CFG_NODE   DT_CHOSEN(auxspace_sm_config)
#define SM_CFG_DEV    PARTITION_NODE_DEVICE(SM_CFG_NODE)
#define SM_CFG_OFFSET PARTITION_NODE_OFFSET(SM_CFG_NODE)

/** @brief A recognisably non-default threshold set. */
static const struct sm_thresholds tall_rocket = {
	.T_AB = 42,
	.T_H = 120,
	.T_BB = 11,
	.T_M = 1500,
	.T_L = 3,
	.T_OA = 80,
	.T_OI = 45,
	.N_OI = 7,
	.DT_AB = 750,
	.DT_L = 600,
	.TO_A = 90000,
	.TO_M = 3000,
	.TO_R = 600000,
};

static void state_config_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_ok(sm_config_erase(), "could not clear the store");
	sm_init(&tall_rocket, NULL);
}

static void state_config_after(void *fixture)
{
	ARG_UNUSED(fixture);

	sm_deinit();
}

ZTEST_SUITE(state_config_tests, NULL, NULL,
	    state_config_before, state_config_after, NULL);

/*-----------------------------------------------------------
 * Defaults
 *----------------------------------------------------------*/

ZTEST(state_config_tests, test_defaults_come_from_kconfig)
{
	struct sm_thresholds def;

	sm_config_defaults(&def);

	zassert_equal(def.T_H, CONFIG_BOOST_ALTITUDE, "T_H should be the Kconfig default");
	zassert_equal(def.T_M, CONFIG_MAIN_DESCENT_HEIGHT, "T_M should be the Kconfig default");
	zassert_equal(def.TO_R, CONFIG_REDUNDANT_TIMEOUT_MS, "TO_R should be the Kconfig default");
}

/*-----------------------------------------------------------
 * Field table
 *----------------------------------------------------------*/

ZTEST(state_config_tests, test_field_table_covers_every_threshold)
{
	size_t count;

	(void)sm_config_fields(&count);

	/* One descriptor per int in struct sm_thresholds; a field added to
	 * the struct without a descriptor would be invisible to the shell.
	 */
	zassert_equal(count, sizeof(struct sm_thresholds) / sizeof(int),
		      "field table does not cover struct sm_thresholds");
}

ZTEST(state_config_tests, test_field_lookup_is_case_insensitive)
{
	const struct sm_config_field *upper = sm_config_field_find("T_M");
	const struct sm_config_field *lower = sm_config_field_find("t_m");

	zassert_not_null(upper, "T_M should be a known threshold");
	zassert_equal(upper, lower, "lookup should ignore case");
}

ZTEST(state_config_tests, test_field_lookup_rejects_unknown)
{
	zassert_is_null(sm_config_field_find("T_BOGUS"), "unknown name should not match");
	zassert_is_null(sm_config_field_find("T_"), "partial name should not match");
}

ZTEST(state_config_tests, test_field_get_set)
{
	const struct sm_config_field *field = sm_config_field_find("T_M");
	struct sm_thresholds cfg = tall_rocket;

	zassert_equal(sm_config_field_get(&cfg, field), tall_rocket.T_M,
		      "get should read the addressed field");

	zassert_ok(sm_config_field_set(&cfg, field, 250), "set should accept an in-range value");
	zassert_equal(cfg.T_M, 250, "set should write the addressed field");
	zassert_equal(cfg.T_H, tall_rocket.T_H, "set should not touch its neighbours");
}

ZTEST(state_config_tests, test_field_set_rejects_out_of_range)
{
	const struct sm_config_field *field = sm_config_field_find("T_OA");
	struct sm_thresholds cfg = tall_rocket;

	zassert_equal(sm_config_field_set(&cfg, field, field->max + 1), -ERANGE,
		      "above-max value should be rejected");
	zassert_equal(sm_config_field_set(&cfg, field, field->min - 1), -ERANGE,
		      "below-min value should be rejected");
	zassert_equal(cfg.T_OA, tall_rocket.T_OA, "a rejected set must not modify the field");
}

/*-----------------------------------------------------------
 * Store round-trip
 *----------------------------------------------------------*/

ZTEST(state_config_tests, test_save_then_load_round_trips)
{
	struct sm_thresholds out;

	zassert_ok(sm_config_save(&tall_rocket), "save failed");

	zassert_ok(sm_config_load(&out), "load should report a stored record");
	zassert_mem_equal(&out, &tall_rocket, sizeof(out),
			  "loaded thresholds differ from the saved ones");
}

ZTEST(state_config_tests, test_load_without_record_falls_back_to_defaults)
{
	struct sm_thresholds out, def;

	sm_config_defaults(&def);

	/* The fixture already erased the page. */
	zassert_equal(sm_config_load(&out), -ENOENT, "empty store should report -ENOENT");
	zassert_mem_equal(&out, &def, sizeof(out), "should have fallen back to the defaults");
}

ZTEST(state_config_tests, test_erase_drops_the_record)
{
	struct sm_thresholds out, def;

	sm_config_defaults(&def);

	zassert_ok(sm_config_save(&tall_rocket), "save failed");
	zassert_ok(sm_config_load(&out), "precondition: record should be readable");

	zassert_ok(sm_config_erase(), "erase failed");

	zassert_equal(sm_config_load(&out), -ENOENT, "erased store should report -ENOENT");
	zassert_mem_equal(&out, &def, sizeof(out), "should have fallen back to the defaults");
}

ZTEST(state_config_tests, test_corrupt_record_falls_back_to_defaults)
{
	const struct device *dev = SM_CFG_DEV;
	struct sm_thresholds out, def;
	/* Zeroes clear bits only, so this lands without an erase - the same
	 * way a power loss part-way through a write would.
	 */
	const uint32_t zeroes = 0;

	sm_config_defaults(&def);

	zassert_ok(sm_config_save(&tall_rocket), "save failed");

	/* Clobber the first threshold, leaving the header (and its CRC) intact. */
	zassert_ok(flash_write(dev, SM_CFG_OFFSET + 16, &zeroes, sizeof(zeroes)),
		   "could not corrupt the record");

	zassert_equal(sm_config_load(&out), -ENOENT, "CRC mismatch should report -ENOENT");
	zassert_mem_equal(&out, &def, sizeof(out), "should have fallen back to the defaults");
}

/*-----------------------------------------------------------
 * Applying thresholds at runtime
 *----------------------------------------------------------*/

ZTEST(state_config_tests, test_set_thresholds_applies_in_idle)
{
	struct sm_thresholds def, running;

	sm_config_defaults(&def);

	zassert_equal(sm_get_state(), SM_IDLE, "precondition: should be IDLE");
	zassert_ok(sm_set_thresholds(&def), "IDLE should accept new thresholds");

	sm_backend_get_thresholds(&running);
	zassert_mem_equal(&running, &def, sizeof(running),
			  "the backend should be running the new thresholds");
}

ZTEST(state_config_tests, test_set_thresholds_refused_outside_idle)
{
	struct sm_thresholds def, running;

	sm_config_defaults(&def);

	sm_update_force(SM_BOOST);
	zassert_equal(sm_set_thresholds(&def), -EBUSY,
		      "thresholds must not change mid-flight");

	sm_backend_get_thresholds(&running);
	zassert_mem_equal(&running, &tall_rocket, sizeof(running),
			  "a refused set must leave the running thresholds alone");
}
