/**
 * @file buzzer.c
 * @brief PWM-Buzzer notification backend.
 *
 * Drives the board's PWM-backed passive buzzer (via @c auxspace_buzzer chosen
 * node) to indicate boot, flight state-machine transitions, calibration
 * completion and error.  Registered at link time as a @ref notify_backend via
 * @ref NOTIFY_BACKEND_DEFINE.
 *
 * All tone sequences are blocking (they interleave @c pwm_set_dt with
 * @c k_sleep); to keep them off the caller's thread (typically the state
 * machine task, which runs at 10 Hz), events are posted to a bounded
 * FIFO and played by a dedicated worker thread. The queue preserves
 * ordering of calls like @ref pwm_melody_stop so that important
 * sequencing (stop-melody-before-play-tone) is never reordered.
 * When the queue is full, newly posted events are dropped with a
 * warning — the worker drains the queue in order, providing natural
 * back-pressure / rate-limiting.
 *
 * Copyright (c) 2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/notify.h>
#include <aurora/lib/pwm_melody.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(notify_buzzer, CONFIG_AURORA_NOTIFY_LOG_LEVEL);

static const struct pwm_dt_spec buzzer =
	PWM_DT_SPEC_GET(DT_CHOSEN(auxspace_buzzer));

PWM_MELODY_CTX_DEFINE(melody_ctx, &buzzer, astronomia, 1024);

/** @brief Buzzer event types. */
enum buzzer_evt_type {
	BUZZER_EVT_BOOT,
	BUZZER_EVT_STATE_CHANGE,
	BUZZER_EVT_CALIBRATION_COMPLETE,
	BUZZER_EVT_ERROR,
};

struct buzzer_evt {
	enum buzzer_evt_type type;
	/** Only valid for @c BUZZER_EVT_STATE_CHANGE. */
	enum sm_state next_state;
};

K_MSGQ_DEFINE(buzzer_msgq, sizeof(struct buzzer_evt),
	      CONFIG_AURORA_NOTIFY_BUZZER_QUEUE_SIZE, 4);
K_THREAD_STACK_DEFINE(buzzer_thread_stack,
		      CONFIG_AURORA_NOTIFY_BUZZER_STACK_SIZE);
static struct k_thread buzzer_thread;

/** @brief Play a tone for @p duration_ms then silence. */
static int buzz(uint32_t period_ns, uint32_t duration_ms)
{
	int ret;

	ret = pwm_set_dt(&buzzer, period_ns, period_ns / 2);
	if (ret) {
		return ret;
	}
	k_sleep(K_MSEC(duration_ms));
	return pwm_set_dt(&buzzer, period_ns, 0);
}

static void play_boot(void)
{
	(void)buzz(PWM_HZ(4000), 500);
}

static void play_calibration_complete(void)
{
	(void)buzz(PWM_HZ(1000), 500);
}

static void play_error(void)
{
	/* Three short high-pitched beeps. */
	for (int i = 0; i < 3; i++) {
		if (buzz(PWM_HZ(4000), 100)) {
			return;
		}
		k_sleep(K_MSEC(100));
	}
}

static void play_state_change(enum sm_state next)
{
	pwm_melody_stop(&melody_ctx);

	switch (next) {
	case SM_ARMED:
		(void)buzz(PWM_HZ(2000), 200);
		break;
	case SM_IDLE:
		(void)buzz(PWM_HZ(500), 50);
		break;
	case SM_APOGEE:
		(void)buzz(PWM_HZ(3000), 300);
		break;
	case SM_MAIN:
		(void)buzz(PWM_HZ(2500), 300);
		break;
	case SM_REDUNDANT:
		/* Two short mid-high beeps: backup deployment active. */
		for (int i = 0; i < 2; i++) {
			if (buzz(PWM_HZ(2500), 150)) {
				return;
			}
			k_sleep(K_MSEC(100));
		}
		break;
	case SM_LANDED:
		(void)pwm_melody_start(&melody_ctx);
		break;
	default:
		break;
	}
}

static void buzzer_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct buzzer_evt evt;

	for (;;) {
		(void)k_msgq_get(&buzzer_msgq, &evt, K_FOREVER);

		switch (evt.type) {
		case BUZZER_EVT_BOOT:
			play_boot();
			break;
		case BUZZER_EVT_STATE_CHANGE:
			play_state_change(evt.next_state);
			break;
		case BUZZER_EVT_CALIBRATION_COMPLETE:
			play_calibration_complete();
			break;
		case BUZZER_EVT_ERROR:
			play_error();
			break;
		}
	}
}

static int enqueue(const struct buzzer_evt *evt)
{
	int ret = k_msgq_put(&buzzer_msgq, evt, K_NO_WAIT);

	if (ret) {
		LOG_WRN("Buzzer queue full, dropping event (type=%d)",
			evt->type);
	}
	return ret;
}

static int buzzer_init(void)
{
	if (!pwm_is_ready_dt(&buzzer)) {
		LOG_ERR("Buzzer PWM device not ready");
		return -ENODEV;
	}

	k_thread_create(&buzzer_thread, buzzer_thread_stack,
			K_THREAD_STACK_SIZEOF(buzzer_thread_stack),
			buzzer_thread_fn, NULL, NULL, NULL,
			CONFIG_AURORA_NOTIFY_BUZZER_THREAD_PRIORITY,
			0, K_NO_WAIT);
	k_thread_name_set(&buzzer_thread, "buzzer_notify");
	return 0;
}

static int buzzer_on_boot(void)
{
	const struct buzzer_evt evt = { .type = BUZZER_EVT_BOOT };

	return enqueue(&evt);
}

static int buzzer_on_state_change(enum sm_state prev, enum sm_state next)
{
	ARG_UNUSED(prev);

	const struct buzzer_evt evt = {
		.type = BUZZER_EVT_STATE_CHANGE,
		.next_state = next,
	};

	return enqueue(&evt);
}

static int buzzer_on_error(void)
{
	const struct buzzer_evt evt = { .type = BUZZER_EVT_ERROR };

	return enqueue(&evt);
}

static int buzzer_on_calibration_complete(void)
{
	const struct buzzer_evt evt = { .type = BUZZER_EVT_CALIBRATION_COMPLETE };

	return enqueue(&evt);
}

static const struct notify_backend_api buzzer_api = {
	.init = buzzer_init,
	.on_boot = buzzer_on_boot,
	.on_state_change = buzzer_on_state_change,
	.on_calibration_complete = buzzer_on_calibration_complete,
	.on_error = buzzer_on_error,
};

NOTIFY_BACKEND_DEFINE(notify_buzzer, &buzzer_api);
