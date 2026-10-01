/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bench shell for the bridge. "abby test" exists so each half can be
 * proven on its own, before the other end of it exists: it pushes a
 * synthetic frame into whichever output the role owns - the radio in the
 * relay role, the stack UART in the receive role.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <aurora/lib/telemetry/wire.h>

#include "downlink.h"
#include "link.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct link_stats ls;
	struct downlink_stats ds;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	link_get_stats(&ls);
	downlink_get_stats(&ds);

	shell_print(sh, "role              : %s",
		    IS_ENABLED(CONFIG_ABBY_ROLE_RELAY)
			    ? "relay (stack UART -> LoRa)"
			    : "receive (LoRa -> stack UART)");

	/* Every one of these has to match at the other end of the link, and
	 * a mismatch is indistinguishable from a dead radio, so make them
	 * easy to compare between two boards.
	 */
	shell_print(sh, "radio             : %u Hz, SF%d, BW%d kHz, CR4/%d, "
			"%s sync",
		    (unsigned int)CONFIG_ABBY_LORA_FREQ_HZ,
		    CONFIG_ABBY_LORA_SF, CONFIG_ABBY_LORA_BW_KHZ,
		    CONFIG_ABBY_LORA_CODING_RATE + 4,
		    IS_ENABLED(CONFIG_ABBY_LORA_PUBLIC_NETWORK) ? "public"
								: "private");

#if defined(CONFIG_ABBY_ROLE_RELAY)
	shell_print(sh, "stack link (in):");
	shell_print(sh, "  frames accepted : %u", ls.frames);
	shell_print(sh, "  CRC errors      : %u", ls.crc_errs);
	shell_print(sh, "  ring overruns   : %u B", ls.overruns);
	shell_print(sh, "lora radio (out):");
	shell_print(sh, "  frames sent     : %u", ds.sent);
	shell_print(sh, "  send errors     : %u", ds.tx_errs);
	shell_print(sh, "  superseded      : %u", ds.superseded);
	shell_print(sh, "  oversize        : %u", ds.oversize);
	shell_print(sh, "  last airtime    : %u ms", ds.last_air_ms);
	shell_print(sh, "  duty cycle      : %s (%d.%d%%)",
		    IS_ENABLED(CONFIG_ABBY_DUTY_CYCLE_ENFORCE) ? "enforced"
							       : "OFF",
		    CONFIG_ABBY_DUTY_CYCLE_PERMILLE / 10,
		    CONFIG_ABBY_DUTY_CYCLE_PERMILLE % 10);
#else
	shell_print(sh, "lora radio (in):");
	shell_print(sh, "  frames received : %u", ds.received);
	shell_print(sh, "  rejected        : %u", ds.rejected);
	shell_print(sh, "  recv errors     : %u", ds.rx_errs);
	shell_print(sh, "  last RSSI       : %d dBm", ds.last_rssi);
	shell_print(sh, "  last SNR        : %d dB", ds.last_snr);
	shell_print(sh, "stack link (out):");
	shell_print(sh, "  frames written  : %u", ls.sent);
#endif

	return 0;
}

static int cmd_test(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t payload_len = 16;
	uint8_t frame[AURORA_TELEMETRY_WIRE_MAX_FRAME];

	if (argc > 1) {
		long n = strtol(argv[1], NULL, 0);

		if (n < 0 || n > AURORA_TELEMETRY_WIRE_MAX_PAYLOAD) {
			shell_error(sh, "payload length out of range (0..%d)",
				    AURORA_TELEMETRY_WIRE_MAX_PAYLOAD);
			return -EINVAL;
		}
		payload_len = (uint8_t)n;
	}

	uint8_t payload[AURORA_TELEMETRY_WIRE_MAX_PAYLOAD];

	for (uint8_t i = 0; i < payload_len; i++) {
		payload[i] = i;
	}

	/* Built with the same helper the senders use, so a frame that
	 * passes here is a frame the parser must accept.
	 */
	size_t len = telemetry_wire_finalise(
		frame, sizeof(frame), AURORA_TELEMETRY_WIRE_TYPE_SM_UPDATE,
		payload, payload_len);

	if (len == 0) {
		shell_error(sh, "frame build failed");
		return -EINVAL;
	}

#if defined(CONFIG_ABBY_ROLE_RELAY)
	int rc = downlink_submit(frame, len);

	if (rc) {
		shell_error(sh, "submit failed (%d)", rc);
		return rc;
	}

	shell_print(sh, "queued a %u byte test frame for the radio; "
			"a duty-cycle wait may delay it", (unsigned int)len);
#else
	int rc = link_send(frame, len);

	if (rc) {
		shell_error(sh, "write failed (%d)", rc);
		return rc;
	}

	shell_print(sh, "wrote a %u byte test frame to the stack UART",
		    (unsigned int)len);
#endif

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(abby_cmds,
	SHELL_CMD(status, NULL, "Show link and radio counters", cmd_status),
	SHELL_CMD_ARG(test, NULL,
		      "Push a synthetic frame into this role's output: "
		      "test [payload_len]",
		      cmd_test, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(abby, &abby_cmds, "AUX-Tel telemetry bridge", NULL);
