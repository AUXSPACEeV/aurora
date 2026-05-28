/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include <aurora/lib/state/state.h>

#include "hc12_internal.h"

/* Bench-provisioning shell. Refuses every command unless the SM is in
 * IDLE: messing with the radio while armed risks losing the downlink
 * during a flight, and the AT exchange stalls the UART for ~200 ms.
 */
static int check_idle(const struct shell *sh)
{
	if (sm_get_state() != SM_IDLE) {
		shell_error(sh, "Refused: state machine not in IDLE "
				"(current: %s)",
			    sm_state_str(sm_get_state()));
		return -EBUSY;
	}
	return 0;
}

/* Default inactivity gap for HC-12 AT replies. The module bursts the
 * whole response in <50 ms; 100 ms is a safe terminator.
 */
#define HC12_AT_QUIET_MS 100

static int run_at(const struct shell *sh, const char *cmd)
{
	char resp[128];
	size_t n = 0;

	int rc = hc12_at_exec(cmd, strlen(cmd), resp, sizeof(resp),
			      HC12_AT_QUIET_MS, &n);
	if (rc == -ENOTSUP) {
		shell_error(sh, "SET pin not wired in DT: runtime AT "
				"unavailable. Provision the HC-12 on the "
				"bench with a USB-serial adapter.");
		return rc;
	}
	if (rc) {
		shell_error(sh, "AT exec failed (%d)", rc);
		return rc;
	}
	if (n == 0) {
		shell_warn(sh, "No response from HC-12");
		return -ETIMEDOUT;
	}
	shell_print(sh, "%s", resp);
	return 0;
}

static int cmd_at(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: telemetry hc12 at <command>");
		return -EINVAL;
	}
	if (check_idle(sh)) {
		return -EBUSY;
	}
	return run_at(sh, argv[1]);
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (check_idle(sh)) {
		return -EBUSY;
	}
	/* AT+RX returns channel, baud, power and FU mode in one burst. */
	return run_at(sh, "AT+RX");
}

static int cmd_channel(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: telemetry hc12 channel <1..127>");
		return -EINVAL;
	}
	if (check_idle(sh)) {
		return -EBUSY;
	}

	int ch = atoi(argv[1]);
	if (ch < 1 || ch > 127) {
		shell_error(sh, "Channel out of range (1..127)");
		return -EINVAL;
	}

	char cmd[16];
	(void)snprintf(cmd, sizeof(cmd), "AT+C%03d", ch);
	return run_at(sh, cmd);
}

static int cmd_baud(const struct shell *sh, size_t argc, char **argv)
{
	static const int allowed[] = {
		1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200,
	};

	if (argc != 2) {
		shell_error(sh, "Usage: telemetry hc12 baud "
				"<1200|2400|4800|9600|19200|38400|57600|115200>");
		return -EINVAL;
	}
	if (check_idle(sh)) {
		return -EBUSY;
	}

	int b = atoi(argv[1]);
	bool ok = false;
	for (size_t i = 0; i < ARRAY_SIZE(allowed); i++) {
		if (allowed[i] == b) {
			ok = true;
			break;
		}
	}
	if (!ok) {
		shell_error(sh, "Unsupported baud %d", b);
		return -EINVAL;
	}

	char cmd[16];
	(void)snprintf(cmd, sizeof(cmd), "AT+B%d", b);
	int rc = run_at(sh, cmd);
	if (rc == 0) {
		shell_warn(sh, "HC-12 air baud is now %d; update DT "
			       "current-speed and reboot to match the "
			       "host UART.", b);
	}
	return rc;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: telemetry hc12 power <1..8>");
		return -EINVAL;
	}
	if (check_idle(sh)) {
		return -EBUSY;
	}

	int p = atoi(argv[1]);
	if (p < 1 || p > 8) {
		shell_error(sh, "Power out of range (1..8; 8 = +20 dBm)");
		return -EINVAL;
	}

	char cmd[8];
	(void)snprintf(cmd, sizeof(cmd), "AT+P%d", p);
	return run_at(sh, cmd);
}

static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: telemetry hc12 mode <1..4>");
		return -EINVAL;
	}
	if (check_idle(sh)) {
		return -EBUSY;
	}

	int m = atoi(argv[1]);
	if (m < 1 || m > 4) {
		shell_error(sh, "FU mode out of range (1..4)");
		return -EINVAL;
	}

	char cmd[8];
	(void)snprintf(cmd, sizeof(cmd), "AT+FU%d", m);
	return run_at(sh, cmd);
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (check_idle(sh)) {
		return -EBUSY;
	}
	int rc = run_at(sh, "AT+DEFAULT");
	if (rc == 0) {
		shell_warn(sh, "HC-12 reset to factory defaults "
			       "(9600 baud, channel 001, P8, FU3). "
			       "Update DT and reboot to match.");
	}
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_hc12,
	SHELL_CMD(info, NULL,
		  "Show channel/baud/power/mode (AT+RX)", cmd_info),
	SHELL_CMD_ARG(channel, NULL,
		      "Set RF channel (1..127)", cmd_channel, 2, 0),
	SHELL_CMD_ARG(baud, NULL,
		      "Set air baud rate (1200..115200)", cmd_baud, 2, 0),
	SHELL_CMD_ARG(power, NULL,
		      "Set TX power level (1..8; 8 = +20 dBm)", cmd_power,
		      2, 0),
	SHELL_CMD_ARG(mode, NULL,
		      "Set FU mode (1..4)", cmd_mode, 2, 0),
	SHELL_CMD(reset, NULL,
		  "Restore HC-12 factory defaults (AT+DEFAULT)", cmd_reset),
	SHELL_CMD_ARG(at, NULL,
		      "Run a raw AT command, e.g. \"AT+V\"", cmd_at, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_telemetry,
	SHELL_CMD(hc12, &sub_hc12, "HC-12 provisioning commands", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(telemetry, &sub_telemetry,
		   "Telemetry backend commands", NULL);
