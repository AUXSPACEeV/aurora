/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * abby - the AUX-Tel telemetry bridge.
 *
 * AUX-Tel sits in the AUX-Stack next to a brain board (AUX-Core) and
 * owns the long-range link. abby is the firmware that makes it a bridge
 * between the stack data connector's UART and the LoRa radio, in
 * whichever direction ABBY_ROLE selects:
 *
 *   relay   - the flight role. Telemetry wire frames arrive from the
 *             brain board over the stack connector, and every frame that
 *             passes its CRC is relayed verbatim over LoRa.
 *   receive - the ground role. Frames arrive over LoRa and every one
 *             that validates is written verbatim to the stack
 *             connector's UART, for a brain board or a USB-serial
 *             adapter to consume.
 *
 * Relaying verbatim is the point. The frames are the same ones the UART
 * link backends in lib/telemetry emit, so a ground station already able
 * to decode an HC-12 downlink needs no change to decode this one, and
 * abby itself never has to learn a payload layout. It also means the two
 * roles compose: a receive-role board feeds a brain board exactly what a
 * directly wired relay would have.
 *
 * The two transports live in link.c (the stack UART) and downlink.c (the
 * radio); each one is a source in one role and a sink in the other.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/app_version.h>

#include "downlink.h"
#include "link.h"

LOG_MODULE_REGISTER(abby, CONFIG_ABBY_LOG_LEVEL);

#define ROLE_NAME (IS_ENABLED(CONFIG_ABBY_ROLE_RELAY) ? "relay" : "receive")

int main(void)
{
	int rc;

	LOG_INF("abby %s on %s, role %s", APP_VERSION_STRING,
		CONFIG_BOARD_TARGET, ROLE_NAME);

	/* Sink before source, in both roles: whichever transport carries
	 * frames out has to be ready before the one that brings them in
	 * starts delivering, or the first frames are dropped.
	 */
#if defined(CONFIG_ABBY_ROLE_RELAY)
	rc = downlink_init();
	if (rc) {
		LOG_ERR("radio init failed (%d)", rc);
		return rc;
	}

	rc = link_init();
	if (rc) {
		LOG_ERR("stack link init failed (%d)", rc);
		return rc;
	}

	LOG_INF("relaying stack telemetry to LoRa");
#else
	rc = link_init();
	if (rc) {
		LOG_ERR("stack link init failed (%d)", rc);
		return rc;
	}

	rc = downlink_init();
	if (rc) {
		LOG_ERR("radio init failed (%d)", rc);
		return rc;
	}

	LOG_INF("relaying LoRa telemetry to the stack connector");
#endif

	/* Everything runs in the link and radio threads. */
	while (1) {
		k_sleep(K_SECONDS(CONFIG_ABBY_HEARTBEAT_SEC));

		if (IS_ENABLED(CONFIG_ABBY_HEARTBEAT_LOG)) {
			struct link_stats ls;
			struct downlink_stats ds;

			link_get_stats(&ls);
			downlink_get_stats(&ds);

			if (IS_ENABLED(CONFIG_ABBY_ROLE_RELAY)) {
				LOG_INF("uart rx %u frames (%u crc, %u overrun) | "
					"lora tx %u (%u err, %u superseded)",
					ls.frames, ls.crc_errs, ls.overruns,
					ds.sent, ds.tx_errs, ds.superseded);
			} else {
				LOG_INF("lora rx %u frames (%u rejected, %u err) | "
					"uart tx %u | last RSSI %d dBm SNR %d dB",
					ds.received, ds.rejected, ds.rx_errs,
					ls.sent, ds.last_rssi, ds.last_snr);
			}
		}
	}

	return 0;
}
