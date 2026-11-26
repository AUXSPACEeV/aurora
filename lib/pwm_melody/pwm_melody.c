/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <aurora/lib/pwm_melody.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pwm_melody, CONFIG_AURORA_PWM_MELODY_LOG_LEVEL);

static void melody_entry(void *p1, void *p2, void *p3)
{
	struct pwm_melody_ctx *ctx = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (ctx->playing) {
		for (size_t i = 0; i < ctx->num_notes && ctx->playing; i++) {
			const struct pwm_melody_note *note = &ctx->notes[i];
			uint32_t note_ms = 750 / note->dur_div;
			uint32_t pause_ms = note_ms * 30 / 100;

			if (note->freq != BUZZER_FREQ_REST) {
				/* The duration makes the driver silence the
				 * note on its own should this thread stall;
				 * the explicit stop keeps the timing exact
				 * when the system workqueue running that
				 * auto-stop is busy (e.g. closing the log).
				 */
				int ret = buzzer_tone(ctx->buzzer, note->freq, note_ms);

				if (ret < 0) {
					LOG_WRN_ONCE("buzzer_tone failed (%d)", ret);
				}
				k_msleep(note_ms);
				(void)buzzer_stop(ctx->buzzer);
			} else {
				k_msleep(note_ms);
			}
			k_msleep(pause_ms);
		}
		if (ctx->playing) {
			k_msleep(500);
		}
	}
}

int pwm_melody_stop(struct pwm_melody_ctx *ctx)
{
	if (ctx->playing) {
		ctx->playing = false;

		/* never recreate over a live thread */
		if (k_thread_join(&ctx->thread, K_SECONDS(5)) != 0) {
			return -EBUSY;
		}
		(void)buzzer_stop(ctx->buzzer);
	}
	return 0;
}

int pwm_melody_start(struct pwm_melody_ctx *ctx)
{
	int ret;

	if (!device_is_ready(ctx->buzzer)) {
		LOG_ERR("Buzzer %s not ready", ctx->buzzer->name);
		return -ENODEV;
	}

	ret = pwm_melody_stop(ctx);
	if (ret != 0) {
		return ret;
	}

	ctx->playing = true;
	k_thread_create(&ctx->thread, ctx->stack,
			ctx->stack_size,
			melody_entry, ctx, NULL, NULL,
			K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
	k_thread_name_set(&ctx->thread, "pwm_melody");
	return 0;
}

int pwm_melody_play(struct pwm_melody_ctx *ctx,
		    const struct pwm_melody_note *notes, size_t num_notes)
{
	int ret;

	if (ctx == NULL || notes == NULL || num_notes == 0) {
		return -EINVAL;
	}

	ret = pwm_melody_stop(ctx);
	if (ret != 0) {
		return ret;
	}

	ctx->notes = notes;
	ctx->num_notes = num_notes;

	return pwm_melody_start(ctx);
}
