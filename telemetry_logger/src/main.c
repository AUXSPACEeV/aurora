/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * telemetry_logger - the AUX-Core ground-side telemetry recorder.
 *
 * On the ground an AUX-Tel running abby in its receive role turns the
 * LoRa downlink back into the telemetry wire byte stream on the AUX-Stack
 * data connector. This application is what sits on the other side of that
 * connector: an AUX-Core that parses the stream and writes every valid
 * frame to the SD card, unconditionally - from boot until power is cut,
 * with no state machine, arming or flight window deciding what is kept.
 *
 * It flies nothing and reads no sensors; sensor_board does that on the
 * vehicle. The two transports live in link.c (the stack UART) and
 * recorder.c (the SD card).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/app_version.h>

#include "link.h"
#include "recorder.h"

LOG_MODULE_REGISTER(tlog, CONFIG_TELEMETRY_LOGGER_LOG_LEVEL);

int main(void)
{
	int rc;

	LOG_INF("telemetry_logger %s on %s", APP_VERSION_STRING,
		CONFIG_BOARD_TARGET);

	/* Sink before source: the recorder has to be taking frames before
	 * the link starts delivering them. It does not wait for the card,
	 * so a board without one still parses and counts.
	 */
	rc = recorder_init();
	if (rc) {
		LOG_ERR("recorder init failed (%d)", rc);
		return rc;
	}

	rc = link_init();
	if (rc) {
		LOG_ERR("stack link init failed (%d)", rc);
		return rc;
	}

	/* Everything runs in the link and recorder threads. */
	while (1) {
		k_sleep(K_SECONDS(CONFIG_TELEMETRY_LOGGER_HEARTBEAT_SEC));

		if (IS_ENABLED(CONFIG_TELEMETRY_LOGGER_HEARTBEAT_LOG)) {
			struct link_stats ls;
			struct recorder_stats rs;

			link_get_stats(&ls);
			recorder_get_stats(&rs);

			LOG_INF("uart rx %u frames (%u crc, %u overrun) | "
				"sd %u recorded, %u unrecorded, %u dropped, "
				"%u err | %s",
				ls.frames, ls.crc_errs, ls.overruns,
				rs.recorded, rs.unrecorded, rs.dropped,
				rs.write_errs, rs.open ? "recording" : "NO CARD");
		}
	}

	return 0;
}
