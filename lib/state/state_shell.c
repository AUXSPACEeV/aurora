/**
 * @file state_shell.c
 * @brief Zephyr shell commands for the state machine.
 *
 * Provides "state_machine status|transition|audit|audit_clear" commands
 * for inspecting and controlling the flight state machine.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

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

/** @brief Show state machine type and current state. */
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Type:  %s", SM_TYPE_NAME);
	shell_print(sh, "State: %s", sm_state_str(sm_get_state()));

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

	/*
	 * Re-initialize to IDLE then, if the target is not IDLE, we
	 * cannot jump arbitrarily because the SM has no public setter.
	 * Instead we deinit + init which lands us in IDLE.
	 */
	sm_deinit();
	shell_print(sh, "%s -> %s (forced via shell)",
		    sm_state_str(current), argv[1]);

	if (target != SM_IDLE) {
		shell_warn(sh, "Only transitions to IDLE are safe; "
			   "state machine has been reset to IDLE");
	}

	return 0;
}

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
