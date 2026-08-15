/**
 * @file state_shell.c
 * @brief Zephyr shell commands for the state machine.
 *
 * Provides "state_machine status|transition|config|audit|audit_clear"
 * commands for inspecting and controlling the flight state machine.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <zephyr/shell/shell.h>

#include <aurora/lib/state/config.h>
#include <aurora/lib/state/state.h>

#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)
#include <aurora/lib/state/audit.h>
#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */

/*-----------------------------------------------------------
 * State machine type name (derived from Kconfig)
 *----------------------------------------------------------*/
#if defined(CONFIG_SIMPLE_STATE)
#define SM_TYPE_NAME "simple"
#else
#define SM_TYPE_NAME "unknown"
#endif

/*-----------------------------------------------------------
 * State name table (for tab completion and parsing)
 *----------------------------------------------------------*/
struct state_entry {
	const char *name;
	enum sm_state state;
};

static const struct state_entry state_table[] = {
	{ "IDLE",	SM_IDLE },
	{ "ARMED",	SM_ARMED },
	{ "BOOST",	SM_BOOST },
	{ "BURNOUT",	SM_BURNOUT },
	{ "APOGEE",	SM_APOGEE },
	{ "MAIN",	SM_MAIN },
	{ "REDUNDANT",	SM_REDUNDANT },
	{ "LANDED",	SM_LANDED },
	{ "ERROR",	SM_ERROR },
};

#define STATE_TABLE_SIZE ARRAY_SIZE(state_table)

static int parse_state(const char *name, enum sm_state *out)
{
	for (size_t i = 0; i < STATE_TABLE_SIZE; i++) {
		if (strcmp(state_table[i].name, name) == 0) {
			*out = state_table[i].state;
			return 0;
		}
	}
	return -EINVAL;
}

/*-----------------------------------------------------------
 * Commands
 *----------------------------------------------------------*/

static enum sm_state forced_target_state;

/** @brief  Callback function to handle the user input for forced state transitions.*/
static void transition_bypass_cb(const struct shell *sh, uint8_t *data, size_t len, void *user_data)
{
	if (len == 0 || data == NULL) return;
	if (data[0] == '\0') return;

	char ch = tolower((char)data[0]);

	if (ch == 'y') {
		shell_print(sh, "Transitioning to state %s...", sm_state_str(forced_target_state));

		sm_update_force(forced_target_state);

	} else if (ch == 'n') {
		shell_warn(sh, "Transition aborted by user.");
	} else {
		shell_warn(sh, "Invalid input. Please enter 'y' or 'n': ");
		return;
	}

	shell_set_bypass(sh, NULL, NULL);
}

/** @brief Show state machine type and current state. */
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct sm_thresholds th;
	struct sm_inputs in;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Type:  %s", SM_TYPE_NAME);
	shell_print(sh, "State: %s", sm_state_str(sm_get_state()));

	/* Every condition the machine needs to leave IDLE, as last seen by
	 * sm_update().  A board that sits in IDLE with calibration done says
	 * nothing about which input is missing; this does.  All-zero inputs
	 * mean sm_update() has not run yet (the app gates it on both sensors
	 * having reported).
	 */
	sm_get_inputs(&in);
	sm_backend_get_thresholds(&th);
	shell_print(sh, "Arm gate (last update):");
	shell_print(sh, "  armed:       %s", in.armed ? "yes" : "NO");
	shell_print(sh, "  log_ready:   %s", in.log_ready ? "yes" : "NO");
	shell_print(sh, "  log_busy:    %s", in.log_busy ? "YES (converting)" : "no");
	shell_print(sh, "  calibrated:  %s", in.calibrated ? "yes" : "NO");
#if defined(CONFIG_SIMPLE_STATE)
	shell_print(sh, "  elevation:   %.1f deg (arm >= %d, disarm < %d)",
		    sm_orientation_elevation_deg(in.orientation),
		    th.T_OA, th.T_OI);
#endif /* CONFIG_SIMPLE_STATE */
	shell_print(sh, "  orientation: %.1f %.1f %.1f deg",
		    in.orientation[0], in.orientation[1], in.orientation[2]);

	return 0;
}

/**
 * @brief Force a state transition.
 *
 * WARNING: This bypasses normal flight logic and is intended for
 * ground testing only.  The state machine must be re-initialized
 * afterwards for safe flight use.
 */
static int cmd_transition(const struct shell *sh, size_t argc, char **argv)
{
	enum sm_state target;
	int rc;

	if (argc != 2) {
		shell_error(sh, "Usage: state_machine transition <STATE>");
		return -EINVAL;
	}

	rc = parse_state(argv[1], &target);
	if (rc) {
		shell_error(sh, "Unknown state '%s'", argv[1]);
		return rc;
	}

	enum sm_state current = sm_get_state();

	if (current == target) {
		shell_warn(sh, "Already in %s", argv[1]);
		return 0;
	}

	forced_target_state = target;

	 if (target != SM_IDLE) {
		shell_warn(sh, "Only transitions to IDLE are safe.");
		shell_warn(sh, "State transitioning can result in pyro charges firing. Continue? (y/n)");

		// Unblock the shell
		shell_set_bypass(sh, transition_bypass_cb, NULL);
		return 0;
	 }

	shell_print(sh, "%s -> %s (forced via shell)",
		    sm_state_str(current), argv[1]);
	sm_update_force(target);


	return 0;
}

/*-----------------------------------------------------------
 * Threshold configuration
 *----------------------------------------------------------*/

/**
 * @brief Apply @p cfg and write it to the threshold store.
 *
 * Applying first means a board with no store still flies the new values for
 * this session; the save failure is reported but not fatal.
 */
static int apply_and_save(const struct shell *sh, const struct sm_thresholds *cfg)
{
	int rc = sm_set_thresholds(cfg);

	if (rc == -EBUSY) {
		shell_error(sh, "Thresholds can only be changed in IDLE (now %s)",
			    sm_state_str(sm_get_state()));
		return rc;
	}

	rc = sm_config_save(cfg);
	if (rc == -ENOTSUP) {
		shell_warn(sh, "Applied, but this board has no threshold store "
			       "- the change is lost on reboot");
		return 0;
	}
	if (rc) {
		shell_error(sh, "Applied, but saving failed (%d)", rc);
		return rc;
	}

	shell_print(sh, "Applied and saved");

	return 0;
}

/** @brief List the running thresholds next to the compiled-in defaults. */
static int cmd_config_show(const struct shell *sh, size_t argc, char **argv)
{
	struct sm_thresholds cur, def;
	const struct sm_config_field *fields;
	size_t count;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sm_backend_get_thresholds(&cur);
	sm_config_defaults(&def);
	fields = sm_config_fields(&count);

	shell_print(sh, "%-8s %10s %10s %-8s %s",
		    "Name", "Value", "Default", "Unit", "Description");
	shell_print(sh, "----------------------------------------------------------------");

	for (size_t i = 0; i < count; i++) {
		int value = sm_config_field_get(&cur, &fields[i]);
		int dflt = sm_config_field_get(&def, &fields[i]);

		shell_print(sh, "%-8s %10d %10d %-8s %s",
			    fields[i].name, value, dflt,
			    fields[i].unit, fields[i].desc);
	}

	return 0;
}

/** @brief Set one threshold, apply it and persist the whole set. */
static int cmd_config_set(const struct shell *sh, size_t argc, char **argv)
{
	const struct sm_config_field *field;
	struct sm_thresholds cfg;
	char *end;
	long value;
	int rc;

	ARG_UNUSED(argc);

	field = sm_config_field_find(argv[1]);
	if (field == NULL) {
		shell_error(sh, "Unknown threshold '%s'", argv[1]);
		return -EINVAL;
	}

	value = strtol(argv[2], &end, 0);
	if (*end != '\0' || end == argv[2]) {
		shell_error(sh, "'%s' is not a number", argv[2]);
		return -EINVAL;
	}

	sm_backend_get_thresholds(&cfg);

	rc = sm_config_field_set(&cfg, field, (int)value);
	if (rc == -ERANGE) {
		shell_error(sh, "%s must be between %d and %d %s",
			    field->name, field->min, field->max, field->unit);
		return rc;
	}

	shell_print(sh, "%s = %ld %s", field->name, value, field->unit);

	return apply_and_save(sh, &cfg);
}

/** @brief Restore the compiled-in defaults and persist them. */
static int cmd_config_default(const struct shell *sh, size_t argc, char **argv)
{
	struct sm_thresholds def;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sm_config_defaults(&def);
	shell_print(sh, "Restoring factory thresholds");

	return apply_and_save(sh, &def);
}

/** @brief Persist the running thresholds as they are. */
static int cmd_config_save(const struct shell *sh, size_t argc, char **argv)
{
	struct sm_thresholds cur;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sm_backend_get_thresholds(&cur);

	rc = sm_config_save(&cur);
	if (rc == -ENOTSUP) {
		shell_error(sh, "This board has no threshold store");
		return rc;
	}
	if (rc) {
		shell_error(sh, "Save failed (%d)", rc);
		return rc;
	}

	shell_print(sh, "Saved");

	return 0;
}

/*-----------------------------------------------------------
 * Dynamic completion for threshold names
 *----------------------------------------------------------*/
static void config_name_get(size_t idx, struct shell_static_entry *entry)
{
	__ASSERT(entry != NULL, "config_name_get: entry is NULL.");

	const struct sm_config_field *fields;
	size_t count;

	entry->handler = NULL;
	entry->subcmd = NULL;
	entry->help = NULL;

	fields = sm_config_fields(&count);
	entry->syntax = idx < count ? fields[idx].name : NULL;
}

SHELL_DYNAMIC_CMD_CREATE(dsub_config_name, config_name_get);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_config,
	SHELL_CMD_ARG(set, &dsub_config_name,
		      "Set a threshold and save it: config set <NAME> <VALUE>",
		      cmd_config_set, 3, 0),
	SHELL_CMD(default, NULL,
		  "Restore the factory thresholds and save them",
		  cmd_config_default),
	SHELL_CMD(save, NULL,
		  "Save the running thresholds", cmd_config_save),
	SHELL_SUBCMD_SET_END);

#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)

/** @brief Dump the audit log. */
static int cmd_audit(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = sm_audit_count();
	struct sm_audit_entry e;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (n == 0) {
		shell_print(sh, "(audit log empty)");
		return 0;
	}

	shell_print(sh, "%-12s %-12s %-12s %s",
		    "Time (ms)", "Type", "From", "To / Event");
	shell_print(sh, "------------------------------------------------");

	for (uint32_t i = 0; i < n; i++) {
		if (sm_audit_get(i, &e) != 0) {
			break;
		}

		if (e.type == SM_AUDIT_TRANSITION) {
			shell_print(sh, "%-12llu %-12s %-12s %s",
				    (unsigned long long)e.timestamp_ns,
				    "transition",
				    sm_state_str(e.from),
				    sm_state_str(e.to));
		} else {
			shell_print(sh, "%-12llu %-12s %-12s %s",
				    (unsigned long long)e.timestamp_ns,
				    "event",
				    sm_state_str(e.from),
				    e.event ? e.event : "");
		}
	}

	return 0;
}

/** @brief Clear the audit log. */
static int cmd_audit_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sm_audit_clear();
	shell_print(sh, "Audit log cleared");

	return 0;
}

#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */

/*-----------------------------------------------------------
 * Dynamic completion for state names
 *----------------------------------------------------------*/
static void state_name_get(size_t idx, struct shell_static_entry *entry)
{
	entry->handler = NULL;
	entry->subcmd = NULL;
	entry->help = NULL;

	if (idx < STATE_TABLE_SIZE) {
		entry->syntax = state_table[idx].name;
	} else {
		entry->syntax = NULL;
	}
}

SHELL_DYNAMIC_CMD_CREATE(dsub_state_name, state_name_get);

/*-----------------------------------------------------------
 * Subcommand tree
 *----------------------------------------------------------*/
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_state_machine,
	SHELL_CMD(status, NULL,
		  "Show state machine type and current state", cmd_status),
	SHELL_CMD_ARG(transition, &dsub_state_name,
		      "Force a state transition (ground test only)",
		      cmd_transition, 2, 0),
	SHELL_CMD(config, &sub_config,
		  "Show the flight thresholds; see subcommands to change them",
		  cmd_config_show),
#if defined(CONFIG_AURORA_STATE_MACHINE_AUDIT)
	SHELL_CMD(audit, NULL,
		  "Show audit log of state transitions and events",
		  cmd_audit),
	SHELL_CMD(audit_clear, NULL,
		  "Clear the audit log", cmd_audit_clear),
#endif /* CONFIG_AURORA_STATE_MACHINE_AUDIT */
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(state_machine, &sub_state_machine,
		   "State machine commands", NULL);
