/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The LoRa radio: whichever end of the AUX-Tel air link this firmware
 * is. The roles are mirror images.
 *
 *   relay   - frames arrive from the stack UART and go out over the air.
 *   receive - frames arrive over the air and go out to the stack UART.
 *
 * Both are built from the same radio Kconfig on purpose. A LoRa packet
 * only demodulates when both ends agree on frequency, spreading factor
 * and bandwidth, and a mismatch is invisible - it looks exactly like no
 * transmitter at all - so the two ends share one set of defaults rather
 * than two that can drift apart.
 *
 * Two properties shape the relay direction:
 *
 *   - Frames go out verbatim. The relay never parses a payload, so the
 *     ground station sees the same bytes whether they arrived over an
 *     HC-12 or over this board, and new packet types need no change
 *     here.
 *   - Airtime, not frame rate, is the scarce resource. A 70-byte frame
 *     at SF7/BW125 is about 130 ms on air, and a 1 % duty cycle then
 *     allows one frame every ~13 s. The transmit worker therefore
 *     budgets from the driver's own lora_airtime() and keeps only the
 *     newest frame while it waits, so what finally goes out is the
 *     freshest state rather than a stale backlog.
 *
 * The receive direction needs none of that: a LoRa packet carries its
 * own boundaries, so a received frame is whole or absent, and there is
 * no duty cycle on listening.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <aurora/lib/telemetry/wire.h>

#include "downlink.h"
#include "link.h"

LOG_MODULE_REGISTER(abby_downlink, CONFIG_ABBY_LOG_LEVEL);

#define LORA_NODE DT_ALIAS(lora0)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE),
	     "No enabled lora0 alias: this application needs a radio");

static const struct device *const lora_dev = DEVICE_DT_GET(LORA_NODE);

/* SX126x-class radios cap a single packet at 255 bytes. */
#define LORA_MAX_PAYLOAD 255

static struct {
	atomic_t sent;
	atomic_t tx_errs;
	atomic_t superseded;
	atomic_t oversize;
	atomic_t last_air_ms;
	atomic_t received;
	atomic_t rejected;
	atomic_t rx_errs;
	atomic_t last_rssi;
	atomic_t last_snr;
} stats;

/* Radio-activity LED. It means the same thing in both roles: this board
 * just used the air. link.c owns led1 for the stack UART side.
 */
#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec radio_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

static inline void radio_led_set(int on)
{
#if DT_NODE_EXISTS(DT_ALIAS(led0))
	if (radio_led.port) {
		(void)gpio_pin_set_dt(&radio_led, on);
	}
#else
	ARG_UNUSED(on);
#endif
}

#if defined(CONFIG_ABBY_ROLE_RELAY)

/* Newest-frame slot. A deeper queue would only let the radio fall
 * further behind real time.
 */
static struct {
	uint8_t buf[AURORA_TELEMETRY_WIRE_MAX_FRAME];
	uint16_t len;
} slot;

static K_MUTEX_DEFINE(slot_lock);
static K_SEM_DEFINE(slot_sem, 0, 1);

int downlink_submit(const uint8_t *frame, size_t len)
{
	if (!frame || len == 0) {
		return -EINVAL;
	}

	if (len > LORA_MAX_PAYLOAD) {
		atomic_inc(&stats.oversize);
		return -EMSGSIZE;
	}

	k_mutex_lock(&slot_lock, K_FOREVER);
	if (slot.len != 0) {
		atomic_inc(&stats.superseded);
	}
	memcpy(slot.buf, frame, len);
	slot.len = (uint16_t)len;
	k_mutex_unlock(&slot_lock);

	k_sem_give(&slot_sem);
	return 0;
}

static void transmit_thread(void *a, void *b, void *c)
{
	/* Earliest uptime at which a transmission may start, from the
	 * duty-cycle budget of the previous one.
	 */
	int64_t next_tx_ms = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		(void)k_sem_take(&slot_sem, K_FOREVER);

		/* Wait out the budget *before* taking a frame, so a frame
		 * that arrives during the wait supersedes the one that
		 * triggered it.
		 */
		if (IS_ENABLED(CONFIG_ABBY_DUTY_CYCLE_ENFORCE)) {
			int64_t wait_ms = next_tx_ms - k_uptime_get();

			if (wait_ms > 0) {
				k_sleep(K_MSEC(wait_ms));
			}
		}

		uint8_t buf[AURORA_TELEMETRY_WIRE_MAX_FRAME];
		uint16_t len;

		k_mutex_lock(&slot_lock, K_FOREVER);
		len = slot.len;
		if (len) {
			memcpy(buf, slot.buf, len);
			slot.len = 0;
		}
		k_mutex_unlock(&slot_lock);

		if (len == 0) {
			continue;
		}

		uint32_t air_ms = lora_airtime(lora_dev, len);
		int64_t started = k_uptime_get();

		radio_led_set(1);
		int rc = lora_send(lora_dev, buf, len);
		radio_led_set(0);

		if (rc < 0) {
			atomic_inc(&stats.tx_errs);
			LOG_ERR("lora_send of %u bytes failed (%d)", len, rc);
		} else {
			atomic_inc(&stats.sent);
			atomic_set(&stats.last_air_ms, (atomic_val_t)air_ms);
			LOG_DBG("sent %u bytes, %u ms airtime", len, air_ms);
		}

		/* The duty-cycle window opens at the start of a
		 * transmission: period = airtime / duty.
		 */
		next_tx_ms = started +
			     (int64_t)air_ms * 1000 /
				     CONFIG_ABBY_DUTY_CYCLE_PERMILLE;
	}
}

K_THREAD_DEFINE(abby_downlink_tid, CONFIG_ABBY_DOWNLINK_STACK_SIZE,
		transmit_thread, NULL, NULL, NULL,
		CONFIG_ABBY_DOWNLINK_THREAD_PRIORITY, 0, 0);

#else /* CONFIG_ABBY_ROLE_RECEIVE */

/* lora_recv() takes its buffer size as a uint8_t, so 255 is the hard
 * ceiling - which is also the largest packet an SX126x can carry, and
 * therefore the largest frame any relay could have put on the air
 * (downlink_submit() rejects more). Passing the 261-byte MAX_FRAME here
 * would silently truncate to 5.
 *
 * File scope rather than stack: only this thread touches it, and it
 * keeps a 255-byte buffer out of the thread's stack budget.
 */
static uint8_t rx_buf[LORA_MAX_PAYLOAD];

BUILD_ASSERT(sizeof(rx_buf) <= UINT8_MAX,
	     "lora_recv() takes the buffer size as a uint8_t");

/* The worker is started by the kernel at boot, before main() has run
 * lora_config(), so it waits here for the radio to be configured. The
 * relay role needs no equivalent: its worker blocks on an empty frame
 * slot anyway. If init fails this is never given and the thread simply
 * never touches the radio.
 */
static K_SEM_DEFINE(radio_ready, 0, 1);

static void rx_led_off(struct k_work *work)
{
	ARG_UNUSED(work);
	radio_led_set(0);
}

static K_WORK_DELAYABLE_DEFINE(rx_led_work, rx_led_off);

static void rx_led_pulse(void)
{
	radio_led_set(1);
	(void)k_work_reschedule(&rx_led_work,
				K_MSEC(CONFIG_ABBY_LED_PULSE_MS));
}

static void receive_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	(void)k_sem_take(&radio_ready, K_FOREVER);

	while (1) {
		int16_t rssi = 0;
		int8_t snr = 0;
		int len = lora_recv(lora_dev, rx_buf, sizeof(rx_buf),
				    K_FOREVER, &rssi, &snr);

		if (len <= 0) {
			if (len < 0) {
				atomic_inc(&stats.rx_errs);
				LOG_ERR("lora_recv failed (%d)", len);
			}
			/* The sleep also keeps this off a hot loop if CAD is
			 * ever configured, where lora_recv() returns 0 as
			 * soon as it finds the channel idle.
			 */
			k_sleep(K_MSEC(100));
			continue;
		}

		/* A LoRa packet is delivered whole, so there is no partial
		 * frame to reassemble: the packet either is a frame or it
		 * is not ours. The radio's own CRC has already rejected
		 * corrupted packets, so anything landing here with a bad
		 * frame CRC is far more likely to be a foreign transmitter
		 * on the same channel.
		 */
		if (!telemetry_wire_validate(rx_buf, (size_t)len)) {
			atomic_inc(&stats.rejected);
			LOG_WRN("rejected a %d byte packet (RSSI %d dBm)",
				len, rssi);
			continue;
		}

		atomic_inc(&stats.received);
		atomic_set(&stats.last_rssi, (atomic_val_t)rssi);
		atomic_set(&stats.last_snr, (atomic_val_t)snr);
		rx_led_pulse();

		LOG_DBG("received frame type %u, %d bytes, RSSI %d dBm, SNR %d dB",
			rx_buf[2], len, rssi, snr);

		int rc = link_send(rx_buf, (size_t)len);

		if (rc) {
			LOG_ERR("forwarding a %d byte frame to the stack UART "
				"failed (%d)", len, rc);
		}
	}
}

K_THREAD_DEFINE(abby_downlink_tid, CONFIG_ABBY_DOWNLINK_STACK_SIZE,
		receive_thread, NULL, NULL, NULL,
		CONFIG_ABBY_DOWNLINK_THREAD_PRIORITY, 0, 0);

#endif /* CONFIG_ABBY_ROLE_* */

void downlink_get_stats(struct downlink_stats *out)
{
	if (!out) {
		return;
	}
	out->sent = (uint32_t)atomic_get(&stats.sent);
	out->tx_errs = (uint32_t)atomic_get(&stats.tx_errs);
	out->superseded = (uint32_t)atomic_get(&stats.superseded);
	out->oversize = (uint32_t)atomic_get(&stats.oversize);
	out->last_air_ms = (uint32_t)atomic_get(&stats.last_air_ms);
	out->received = (uint32_t)atomic_get(&stats.received);
	out->rejected = (uint32_t)atomic_get(&stats.rejected);
	out->rx_errs = (uint32_t)atomic_get(&stats.rx_errs);
	out->last_rssi = (int16_t)atomic_get(&stats.last_rssi);
	out->last_snr = (int8_t)atomic_get(&stats.last_snr);
}

int downlink_init(void)
{
	struct lora_modem_config config = {
		.frequency = CONFIG_ABBY_LORA_FREQ_HZ,
		.bandwidth = (enum lora_signal_bandwidth)CONFIG_ABBY_LORA_BW_KHZ,
		.datarate = (enum lora_datarate)CONFIG_ABBY_LORA_SF,
		.coding_rate = (enum lora_coding_rate)CONFIG_ABBY_LORA_CODING_RATE,
		.preamble_len = CONFIG_ABBY_LORA_PREAMBLE_LEN,
		.tx_power = CONFIG_ABBY_LORA_TX_POWER_DBM,
		.tx = IS_ENABLED(CONFIG_ABBY_ROLE_RELAY),
		.iq_inverted = false,
		.public_network = IS_ENABLED(CONFIG_ABBY_LORA_PUBLIC_NETWORK),
	};

	if (!device_is_ready(lora_dev)) {
		LOG_ERR("radio %s not ready", lora_dev->name);
		return -ENODEV;
	}

#if DT_NODE_EXISTS(DT_ALIAS(led0))
	if (radio_led.port && gpio_is_ready_dt(&radio_led)) {
		(void)gpio_pin_configure_dt(&radio_led, GPIO_OUTPUT_INACTIVE);
	}
#endif

	int rc = lora_config(lora_dev, &config);

	if (rc < 0) {
		LOG_ERR("lora_config failed (%d)", rc);
		return rc;
	}

	/* Logged on one line, and deliberately verbose: every one of these
	 * has to match at the other end, and this is the cheapest way to
	 * compare two boards.
	 */
	LOG_INF("radio up for %s: %u Hz, SF%d, BW%d kHz, CR4/%d, %s sync%s",
		IS_ENABLED(CONFIG_ABBY_ROLE_RELAY) ? "transmit" : "receive",
		(unsigned int)CONFIG_ABBY_LORA_FREQ_HZ,
		CONFIG_ABBY_LORA_SF, CONFIG_ABBY_LORA_BW_KHZ,
		CONFIG_ABBY_LORA_CODING_RATE + 4,
		IS_ENABLED(CONFIG_ABBY_LORA_PUBLIC_NETWORK) ? "public"
							    : "private",
		IS_ENABLED(CONFIG_ABBY_ROLE_RELAY) ? "" : ", listening");

#if defined(CONFIG_ABBY_ROLE_RELAY)
	LOG_INF("transmitting at %d dBm", CONFIG_ABBY_LORA_TX_POWER_DBM);

	if (IS_ENABLED(CONFIG_ABBY_DUTY_CYCLE_ENFORCE)) {
		LOG_INF("duty cycle capped at %d.%d%%",
			CONFIG_ABBY_DUTY_CYCLE_PERMILLE / 10,
			CONFIG_ABBY_DUTY_CYCLE_PERMILLE % 10);
	} else {
		LOG_WRN("duty-cycle budget DISABLED - bench use only");
	}
#else
	/* Release the receive worker now that the radio is configured. */
	k_sem_give(&radio_ready);
#endif

	return 0;
}
