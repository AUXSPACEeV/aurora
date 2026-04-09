/**
 * @file data_shell.c
 * @brief Zephyr shell commands for data logger management.
 *
 * Provides "data_logger list|start|stop|status|flush" commands that
 * operate on loggers registered automatically by data_logger_init().
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include <aurora/lib/data_logger.h>

static const char *fmt_name(const struct data_logger *logger)
{
	if (logger->fmt == NULL) {
		return "n/a";
	}
	return logger->fmt->name;
}

static const char *logger_state_str(const struct data_logger *logger)
{
	if (logger->state == NULL) {
		return "closed";
	}
	return logger->state->running ? "running" : "stopped";
}

struct list_ctx {
	const struct shell *sh;
};

static void list_cb(struct data_logger *logger, void *user_data)
{
	struct list_ctx *ctx = user_data;

	shell_print(ctx->sh, "%-16s %-12s %s",
		    logger->name,
		    fmt_name(logger),
		    logger_state_str(logger));
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	struct list_ctx ctx = { .sh = sh };

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "%-16s %-12s %s", "Name", "Format", "State");
	shell_print(sh, "--------------------------------------");

	data_logger_foreach(list_cb, &ctx);

	return 0;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	struct data_logger *logger;
	int rc;

	if (argc != 2) {
		shell_error(sh, "Usage: data_logger start <name>");
		return -EINVAL;
	}

	logger = data_logger_get(argv[1]);
	if (logger == NULL) {
		shell_error(sh, "Logger '%s' not found", argv[1]);
		return -ENOENT;
	}

	rc = data_logger_start(logger);
	if (rc) {
		shell_error(sh, "Start failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Logger '%s' started", argv[1]);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	struct data_logger *logger;
	int rc;

	if (argc != 2) {
		shell_error(sh, "Usage: data_logger stop <name>");
		return -EINVAL;
	}

	logger = data_logger_get(argv[1]);
	if (logger == NULL) {
		shell_error(sh, "Logger '%s' not found", argv[1]);
		return -ENOENT;
	}

	rc = data_logger_stop(logger);
	if (rc) {
		shell_error(sh, "Stop failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Logger '%s' stopped", argv[1]);
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct data_logger *logger;

	if (argc != 2) {
		shell_error(sh, "Usage: data_logger status <name>");
		return -EINVAL;
	}

	logger = data_logger_get(argv[1]);
	if (logger == NULL) {
		shell_error(sh, "Logger '%s' not found", argv[1]);
		return -ENOENT;
	}

	shell_print(sh, "Logger '%s': %s", argv[1], logger_state_str(logger));
	return 0;
}

static int cmd_flush(const struct shell *sh, size_t argc, char **argv)
{
	struct data_logger *logger;
	int rc;

	if (argc != 2) {
		shell_error(sh, "Usage: data_logger flush <name>");
		return -EINVAL;
	}

	logger = data_logger_get(argv[1]);
	if (logger == NULL) {
		shell_error(sh, "Logger '%s' not found", argv[1]);
		return -ENOENT;
	}

	rc = data_logger_flush(logger);
	if (rc) {
		shell_error(sh, "Flush failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Logger '%s' flushed", argv[1]);
	return 0;
}

struct dynamic_ctx {
	size_t idx;
	size_t target;
	const char *result;
};

static void dynamic_cb(struct data_logger *logger, void *user_data)
{
	struct dynamic_ctx *ctx = user_data;

	if (ctx->result == NULL && ctx->idx == ctx->target) {
		ctx->result = logger->name;
	}
	ctx->idx++;
}

static void logger_name_get(size_t idx, struct shell_static_entry *entry)
{
	struct dynamic_ctx ctx = { .idx = 0, .target = idx, .result = NULL };

	data_logger_foreach(dynamic_cb, &ctx);

	entry->syntax = ctx.result;
	entry->handler = NULL;
	entry->subcmd = NULL;
	entry->help = NULL;
}

SHELL_DYNAMIC_CMD_CREATE(dsub_logger_name, logger_name_get);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_data_logger,
	SHELL_CMD(list, NULL, "List all registered data loggers", cmd_list),
	SHELL_CMD_ARG(start, &dsub_logger_name,
		      "Start a data logger", cmd_start, 2, 0),
	SHELL_CMD_ARG(stop, &dsub_logger_name,
		      "Stop a data logger", cmd_stop, 2, 0),
	SHELL_CMD_ARG(status, &dsub_logger_name,
		      "Show state of a data logger", cmd_status, 2, 0),
	SHELL_CMD_ARG(flush, &dsub_logger_name,
		      "Flush a data logger to storage", cmd_flush, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(data_logger, &sub_data_logger,
		   "Data logger commands", NULL);
