/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file watchdog.c
 * @brief Hardware watchdog supervision of kernel liveness.
 *
 * See watchdog.h for what this deliberately does and does not supervise.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/task_wdt/task_wdt.h>

#include <aurora/lib/watchdog.h>

LOG_MODULE_REGISTER(aurora_watchdog, CONFIG_AURORA_WATCHDOG_LOG_LEVEL);

/* Build-time dependency: the board must expose a hardware watchdog as
 * watchdog0, otherwise the task watchdog has nothing to fall back on and
 * the supervisor shares the fate of whatever wedged the kernel. */
#if !DT_HAS_ALIAS(watchdog0)
#error "CONFIG_AURORA_WATCHDOG requires a 'watchdog0' alias pointing at a hardware watchdog."
#endif

static const struct device *const hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static int wdt_channel = -1;

#define WDT_ISR_FEEDS_ITSELF \
	DT_NODE_HAS_COMPAT(DT_ALIAS(watchdog0), espressif_esp32_watchdog)

#if defined(CONFIG_AURORA_WATCHDOG_BREADCRUMB) && WDT_ISR_FEEDS_ITSELF

#define WDT_BITE_MARK 0x42495445u /* "BITE" */

/* Kept out of the CRC-protected record on purpose: wdt_bite runs from IRAM
 * during the bite and must not call flash-resident code, which rules out
 * recomputing the CRC there.  A bare magic costs one store.
 */
static uint32_t bite_mark __attribute__((section(".rtc_noinit")));
static bool prev_bite_ran;

#define WDT_BITE_STAMP() do { bite_mark = WDT_BITE_MARK; } while (0)
#else
#define WDT_BITE_STAMP() do { } while (0)
#endif

#if WDT_ISR_FEEDS_ITSELF

static void __attribute__((section(".iram1.aurora_wdt_bite"), noinline))
wdt_bite(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);

	/* Whether this callback runs at all is the diagnosis.  Stage 0 is an
	 * interrupt, so reaching here proves the core was still taking them
	 * and the kernel alone was wedged.  Not reaching here proves the
	 * opposite: interrupts masked, or a spin at or above this level, with
	 * only the stage-1 hardware reset able to recover the board.
	 */
	WDT_BITE_STAMP();

	while (true) {
		/* Spin until the reset stage fires. */
	}
}
#endif

#if defined(CONFIG_AURORA_WATCHDOG_BREADCRUMB)

#define WDT_CRUMB_MAGIC   0x57445443u /* "WDTC" */
#define WDT_CRUMB_VERSION 1u

/**
 * @brief What the feeder knew just before the board stopped.
 *
 * @c crc covers every byte after itself, so keep it last in the header and
 * all payload behind it -- same discipline as struct sm_retain_record, which
 * shares this memory.
 */
struct aurora_wdt_crumb {
	uint32_t magic;
	uint16_t version;
	uint16_t size; /**< sizeof(record), rejects a silent layout change. */
	uint32_t crc;  /**< CRC-32/IEEE over everything after this field.   */

	/* --- payload --- */
	uint32_t uptime_ms;   /**< Uptime at the last stamp.                  */
	uint32_t feeds;       /**< Feeds completed this boot.                 */
	uint32_t last_gap_ms; /**< Interval between the last two feeds.       */
	uint32_t max_gap_ms;  /**< Worst interval seen this boot.             */
	int32_t feed_rc;      /**< Return of the last task_wdt_feed().        */
	uint32_t silent_ms[AURORA_WDT_SRC_COUNT]; /**< Per-source silence.    */
};

/* Deliberately uninitialised: .rtc_noinit is NOLOAD and startup does not
 * clear it, which is the whole point.  An initialiser would move this to a
 * loaded section and zero it every boot.
 */
static struct aurora_wdt_crumb crumb __attribute__((section(".rtc_noinit")));

/* Snapshot taken before the feeder starts overwriting the record. */
static struct aurora_wdt_crumb prev_crumb;
static bool prev_crumb_valid;

static uint32_t last_kick_ms[AURORA_WDT_SRC_COUNT];
static uint32_t last_feed_ms;

static uint32_t crumb_crc(const struct aurora_wdt_crumb *rec)
{
	const uint8_t *payload = (const uint8_t *)rec +
				 offsetof(struct aurora_wdt_crumb, crc) + sizeof(rec->crc);
	const size_t len = sizeof(*rec) - offsetof(struct aurora_wdt_crumb, crc) -
			   sizeof(rec->crc);

	return crc32_ieee(payload, len);
}

static bool crumb_valid(const struct aurora_wdt_crumb *rec)
{
	return rec->magic == WDT_CRUMB_MAGIC &&
	       rec->version == WDT_CRUMB_VERSION &&
	       rec->size == sizeof(*rec) &&
	       rec->crc == crumb_crc(rec);
}

/* Must run before the feeder thread starts, or the record it is meant to
 * preserve is the one this boot is already writing.
 */
static void crumb_init(void)
{
	prev_crumb_valid = crumb_valid(&crumb);
	if (prev_crumb_valid) {
		memcpy(&prev_crumb, &crumb, sizeof(prev_crumb));
	}

#if WDT_ISR_FEEDS_ITSELF
	prev_bite_ran = (bite_mark == WDT_BITE_MARK);
	bite_mark = 0u;
#endif

	/* Cold boot leaves RTC RAM full of garbage, so start from a known
	 * zero rather than incrementing whatever was there.
	 */
	memset(&crumb, 0, sizeof(crumb));
}

static void crumb_stamp(int feed_rc)
{
	const uint32_t now = k_uptime_get_32();
	const uint32_t gap = (crumb.feeds == 0u) ? 0u : (now - last_feed_ms);

	last_feed_ms = now;

	crumb.magic = WDT_CRUMB_MAGIC;
	crumb.version = WDT_CRUMB_VERSION;
	crumb.size = sizeof(crumb);
	crumb.uptime_ms = now;
	crumb.feeds++;
	crumb.last_gap_ms = gap;
	crumb.max_gap_ms = MAX(crumb.max_gap_ms, gap);
	crumb.feed_rc = feed_rc;

	for (unsigned int i = 0; i < AURORA_WDT_SRC_COUNT; i++) {
		/* A source that has never kicked leaves last_kick_ms at 0, so
		 * this reads as "silent for the whole boot" -- which it was.
		 */
		crumb.silent_ms[i] = now - last_kick_ms[i];
	}

	crumb.crc = crumb_crc(&crumb);
}

/* aurora_watchdog_kick - see watchdog.h */
void aurora_watchdog_kick(enum aurora_wdt_source src)
{
	if ((unsigned int)src >= AURORA_WDT_SRC_COUNT) {
		return;
	}

	/* No lock: this sits in the sensor hot path, the store is a single
	 * aligned word, and the worst a race can do is mis-date a diagnostic.
	 */
	last_kick_ms[src] = k_uptime_get_32();
}

/* aurora_watchdog_report - see watchdog.h */
void aurora_watchdog_report(void)
{
	static const char *const src_name[AURORA_WDT_SRC_COUNT] = {
		[AURORA_WDT_SRC_IMU] = "imu",
		[AURORA_WDT_SRC_BARO] = "baro",
		[AURORA_WDT_SRC_STATE] = "state",
	};

#if WDT_ISR_FEEDS_ITSELF
	/* Independent of the record below, and the sharper of the two signals:
	 * it says whether the core was still taking interrupts at the end.
	 */
	if (prev_bite_ran) {
		LOG_WRN("breadcrumb: stage-0 bite ran, so the core was still "
			"taking interrupts -- the kernel was wedged, not the CPU");
	} else {
		LOG_WRN("breadcrumb: stage-0 bite never ran, so interrupts were "
			"masked or the core was spinning at or above its level; "
			"only the stage-1 hardware reset could recover it");
	}
#endif

	if (!prev_crumb_valid) {
		LOG_INF("no watchdog breadcrumb retained: cold boot, or the "
			"previous boot never reached the first feed");
		return;
	}

	LOG_WRN("breadcrumb: last fed %u ms into the previous boot "
		"after %u feeds (rc %d)",
		prev_crumb.uptime_ms, prev_crumb.feeds, prev_crumb.feed_rc);
	LOG_WRN("breadcrumb: feed gap last %u ms, worst %u ms, nominal %d ms",
		prev_crumb.last_gap_ms, prev_crumb.max_gap_ms,
		CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS);

	for (unsigned int i = 0; i < AURORA_WDT_SRC_COUNT; i++) {
		LOG_WRN("breadcrumb: %-5s silent for %u ms at that last feed",
			src_name[i], prev_crumb.silent_ms[i]);
	}

	/* The discriminator worth spelling out, because it splits the two
	 * failure modes that look identical from the reset cause alone.
	 * Measured against the timeout rather than the feed interval: what
	 * matters is how much margin was left, not how closely the feeder kept
	 * its nominal period.
	 */
	if (prev_crumb.max_gap_ms < CONFIG_AURORA_WATCHDOG_TIMEOUT_MS / 2u) {
		LOG_WRN("breadcrumb: worst gap stayed under half the %d ms "
			"timeout, so the feeder still had margin when the board "
			"stopped -- an abrupt total halt (double fault or parked "
			"clock), not a thread starving it. Check the ROM's "
			"'Saved PC' line above for where it was spinning",
			CONFIG_AURORA_WATCHDOG_TIMEOUT_MS);
	}
}

#else /* !CONFIG_AURORA_WATCHDOG_BREADCRUMB */

static inline void crumb_init(void)
{
}

static inline void crumb_stamp(int feed_rc)
{
	ARG_UNUSED(feed_rc);
}

#endif /* CONFIG_AURORA_WATCHDOG_BREADCRUMB */

static void wdt_feeder(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		/* Unconditional, whatever the breadcrumb says about silent
		 * sources.  A wedged sensor is a degraded flight to fly
		 * through, and an I2C wedge survives the warm reset anyway, so
		 * withholding the feed here would reset-loop instead of
		 * recovering.  The kicks are evidence, never a vote.
		 */
		int rc = task_wdt_feed(wdt_channel);

		crumb_stamp(rc);
		k_sleep(K_MSEC(CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS));
	}
}

K_THREAD_DEFINE(aurora_wdt_feeder, CONFIG_AURORA_WATCHDOG_STACK_SIZE,
		wdt_feeder, NULL, NULL, NULL,
		CONFIG_AURORA_WATCHDOG_PRIORITY, 0, K_TICKS_FOREVER);

/* aurora_watchdog_setup - see watchdog.h */
int aurora_watchdog_setup(void)
{
	int rc;

	/* Ahead of every early return: the record from the previous boot is
	 * the most useful thing this module owns, and a setup failure is
	 * exactly when it is worth having.
	 */
	crumb_init();
	aurora_watchdog_report();

	if (!device_is_ready(hw_wdt)) {
		LOG_ERR("hardware watchdog %s not ready; kernel liveness is "
			"unsupervised", hw_wdt->name);
		return -ENODEV;
	}

	rc = task_wdt_init(hw_wdt);
	if (rc != 0) {
		LOG_ERR("task watchdog init failed (%d)", rc);
		return rc;
	}

#if WDT_ISR_FEEDS_ITSELF
	/* Overwrite the fallback timeout task_wdt_init() just installed, both to
	 * attach the biting callback and to widen the window.  task_wdt sizes it
	 * at TASK_WDT_MIN_TIMEOUT + TASK_WDT_HW_FALLBACK_DELAY against a feed
	 * that arrives every TASK_WDT_MIN_TIMEOUT, leaving 20 ms of margin -- a
	 * budget this application does not keep during boot, and never had to
	 * while the driver ISR was feeding the watchdog for it.
	 *
	 * Must land between init and add: add() is what calls wdt_setup() and
	 * programs the stages into the hardware. */
	BUILD_ASSERT(CONFIG_AURORA_WATCHDOG_HW_TIMEOUT_MS >
		     2 * CONFIG_TASK_WDT_MIN_TIMEOUT,
		     "hardware backstop must sit well behind the task watchdog's "
		     "feed period, or ordinary stalls reset the board");
	{
		const struct wdt_timeout_cfg bite_cfg = {
			.window = {
				.min = 0U,
				.max = CONFIG_AURORA_WATCHDOG_HW_TIMEOUT_MS,
			},
			.callback = wdt_bite,
			.flags = WDT_FLAG_RESET_SOC,
		};

		rc = wdt_install_timeout(hw_wdt, &bite_cfg);
		if (rc < 0) {
			LOG_ERR("could not arm the hardware watchdog bite (%d); "
				"a kernel freeze would not reset the board", rc);
			return rc;
		}
	}
#endif

	rc = task_wdt_add(CONFIG_AURORA_WATCHDOG_TIMEOUT_MS, NULL, NULL);
	if (rc < 0) {
		LOG_ERR("could not register watchdog channel (%d)", rc);
		return rc;
	}
	wdt_channel = rc;

	k_thread_start(aurora_wdt_feeder);

	LOG_INF("watchdog armed: %d ms timeout, fed every %d ms",
		CONFIG_AURORA_WATCHDOG_TIMEOUT_MS,
		CONFIG_AURORA_WATCHDOG_FEED_INTERVAL_MS);
#if WDT_ISR_FEEDS_ITSELF
	LOG_INF("hardware backstop at %d ms, SoC reset at %d ms",
		CONFIG_AURORA_WATCHDOG_HW_TIMEOUT_MS,
		2 * CONFIG_AURORA_WATCHDOG_HW_TIMEOUT_MS);
#endif

	return 0;
}
