/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_CONFIG_H_
#define APP_LIB_STATE_CONFIG_H_

#include <stddef.h>

#include <aurora/lib/state/state.h>

/**
 * @defgroup lib_state_config Flight threshold store
 * @ingroup lib_state
 * @{
 *
 * @brief Per-vehicle flight thresholds, persisted across reboots.
 *
 * The Kconfig values are the factory defaults.  A vehicle that flies to a
 * different altitude overrides individual thresholds at runtime (see the
 * "state_machine config" shell commands); the overridden set is written to
 * the flash page selected by the 'auxspace,sm-config' chosen node and read
 * back on the next boot by @ref sm_config_load.
 */

/** @brief One editable threshold, for generic (shell) access. */
struct sm_config_field {
	const char *name;  /**< Field name as used by the shell. */
	const char *unit;  /**< Unit shown to the operator. */
	const char *desc;  /**< One-line description. */
	size_t offset;     /**< Byte offset into struct sm_thresholds. */
	int min;           /**< Lowest accepted value. */
	int max;           /**< Highest accepted value. */
};

/**
 * @brief Fill @p out with the compile-time (Kconfig) defaults.
 *
 * @param out Destination, must be non-NULL.
 */
void sm_config_defaults(struct sm_thresholds *out);

/**
 * @brief Load the persisted thresholds, falling back to the defaults.
 *
 * @p out is always left with a usable threshold set, so the caller can pass
 * it straight to @ref sm_init and only treat the return value as diagnostics.
 *
 * @param out Destination, must be non-NULL.
 * @retval 0 A stored record was found and used.
 * @retval -ENOENT No (or no valid) record stored, defaults used.
 * @retval -ENOTSUP No threshold store configured, defaults used.
 * @retval -errno Read failed, defaults used.
 */
int sm_config_load(struct sm_thresholds *out);

/**
 * @brief Persist @p cfg, replacing whatever was stored before.
 *
 * @param cfg Thresholds to store, must be non-NULL.
 * @retval 0 on success.
 * @retval -ENOTSUP No threshold store configured.
 * @retval -errno on erase/write failure.
 */
int sm_config_save(const struct sm_thresholds *cfg);

/**
 * @brief Drop the persisted record, so the next boot uses the defaults.
 *
 * @retval 0 on success, -ENOTSUP without a store, negative errno on failure.
 */
int sm_config_erase(void);

/**
 * @brief Table of editable thresholds.
 *
 * @param count Written with the number of entries, must be non-NULL.
 * @return Pointer to a static table of @p count entries.
 */
const struct sm_config_field *sm_config_fields(size_t *count);

/**
 * @brief Look up a threshold by name (case-insensitive).
 *
 * @param name Field name, e.g. "T_M".
 * @return Matching entry, or NULL if @p name is unknown.
 */
const struct sm_config_field *sm_config_field_find(const char *name);

/**
 * @brief Read a threshold through its descriptor.
 *
 * @param cfg   Threshold set to read from.
 * @param field Descriptor from @ref sm_config_fields.
 * @return The stored value.
 */
int sm_config_field_get(const struct sm_thresholds *cfg,
			const struct sm_config_field *field);

/**
 * @brief Write a threshold through its descriptor, with range checking.
 *
 * @param cfg   Threshold set to modify.
 * @param field Descriptor from @ref sm_config_fields.
 * @param value New value.
 * @retval 0 on success, -ERANGE if @p value is outside the field's range.
 */
int sm_config_field_set(struct sm_thresholds *cfg,
			const struct sm_config_field *field, int value);

/** @} */

#endif /* APP_LIB_STATE_CONFIG_H_ */
