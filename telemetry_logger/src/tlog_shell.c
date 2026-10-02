/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * "tlog" shell commands for bring-up.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <aurora/lib/telemetry/wire.h>

#include "link.h"
#include "recorder.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct link_stats ls;
	struct recorder_stats rs;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	link_get_stats(&ls);
	recorder_get_stats(&rs);

	shell_print(sh, "link:     %u frames, %u crc errors, %u bytes overrun",
		    ls.frames, ls.crc_errs, ls.overruns);
	if (rs.open) {
		shell_print(sh, "recorder: %s/RX_%04u.BIN, %llu bytes",
			    CONFIG_TELEMETRY_LOGGER_DIR, rs.index,
			    (unsigned long long)rs.bytes);
	} else {
		shell_print(sh, "recorder: no recording open, retrying every %d ms",
			    CONFIG_TELEMETRY_LOGGER_RETRY_MS);
	}
	shell_print(sh, "          %u recorded, %u unrecorded, %u dropped, "
		    "%u write errors, %u sessions",
		    rs.recorded, rs.unrecorded, rs.dropped, rs.write_errs,
		    rs.sessions);

	return 0;
}

/* Records a STATUS frame with every flag clear, so the SD path can be
 * proven with nothing on the stack connector. It goes through the same
 * queue as a received frame and lands in both files.
 */
static int cmd_test(const struct shell *sh, size_t argc, char **argv)
{
	struct telemetry_wire_status st = {
		.timestamp_ms = k_uptime_get_32(),
	};
	uint8_t frame[AURORA_TELEMETRY_WIRE_OVERHEAD + sizeof(st)];
	size_t len;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	len = telemetry_wire_finalise(frame, sizeof(frame),
				      AURORA_TELEMETRY_WIRE_TYPE_STATUS,
				      &st, sizeof(st));

	rc = recorder_submit(frame, len);
	if (rc) {
		shell_error(sh, "submit failed (%d)", rc);
		return rc;
	}

	shell_print(sh, "queued a %u byte STATUS frame", (unsigned int)len);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(tlog_cmds,
	SHELL_CMD(status, NULL, "Link and recorder counters", cmd_status),
	SHELL_CMD(test, NULL, "Record a synthetic STATUS frame", cmd_test),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(tlog, &tlog_cmds, "telemetry_logger commands", NULL);
