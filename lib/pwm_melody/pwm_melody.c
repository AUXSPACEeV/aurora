/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <aurora/lib/pwm_melody.h>

static void melody_entry(void *p1, void *p2, void *p3)
{
	struct pwm_melody_ctx *ctx = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (ctx->playing) {
		for (int i = 0; i < ctx->num_notes && ctx->playing; i++) {
			uint32_t note_ms = 750 / ctx->notes[i].dur_div;
			uint32_t pause_ms = note_ms * 30 / 100;

			if (ctx->notes[i].freq > 0) {
				pwm_set_dt(ctx->pwm, PWM_HZ(ctx->notes[i].freq),
					   PWM_HZ(ctx->notes[i].freq) / 2);
				k_sleep(K_MSEC(note_ms));
				pwm_set_dt(ctx->pwm, PWM_HZ(ctx->notes[i].freq), 0);
			} else {
				k_sleep(K_MSEC(note_ms));
			}
			k_sleep(K_MSEC(pause_ms));
		}
		if (ctx->playing) {
			k_sleep(K_MSEC(500));
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
		pwm_set_dt(ctx->pwm, PWM_HZ(1000), 0);
	}
	return 0;
}

int pwm_melody_start(struct pwm_melody_ctx *ctx)
{
	int ret = pwm_melody_stop(ctx);
	if (ret != 0) {
		return ret;
	}

	ctx->playing = true;
	k_thread_create(&ctx->thread, ctx->stack,
			ctx->stack_size,
			melody_entry, ctx, NULL, NULL,
			K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
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
