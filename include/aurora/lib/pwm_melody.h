/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_PWM_MELODY_H_
#define APP_LIB_PWM_MELODY_H_

#include <inttypes.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

struct pwm_melody_note {
	uint16_t freq;
	uint8_t  dur_div;
};

struct pwm_melody_ctx {
	const struct pwm_dt_spec *pwm;
	const struct pwm_melody_note *notes;
	size_t num_notes;
	struct k_thread thread;
	k_thread_stack_t *stack;
	size_t stack_size;
	volatile bool playing;
};

#define PWM_MELODY_CTX_DEFINE(_name, _pwm, _notes, _stack_size)                \
	static K_THREAD_STACK_DEFINE(_name##_stack, _stack_size);               \
	static struct pwm_melody_ctx _name = {                                  \
		.pwm = _pwm,                                                    \
		.notes = _notes,                                                \
		.num_notes = ARRAY_SIZE(_notes),                                \
		.stack = _name##_stack,                                         \
		.stack_size = _stack_size,                                      \
	}

int pwm_melody_start(struct pwm_melody_ctx *ctx);
void pwm_melody_stop(struct pwm_melody_ctx *ctx);

static const struct pwm_melody_note astronomia[] = {
	{466,4},{466,4},{466,4},{466,4},
	{466,4},{466,4},{466,4},{466,4},
	{466,4},{466,4},{466,4},{466,4},
	{466,4},{466,4},{466,4},{466,4},
	{466,4},{466,4},{466,4},{466,4},
	{587,4},{587,4},{587,4},{587,4},
	{523,4},{523,4},{523,4},{523,4},
	{698,4},{698,4},{698,4},{698,4},
	{784,4},{784,4},{784,4},{784,4},
	{784,4},{784,4},{784,4},{784,4},
	{784,4},{784,4},{784,4},{784,4},
	{523,4},{466,4},{440,4},{349,4},
	{392,4},{  0,4},{392,4},{587,4},
	{523,4},{  0,4},{466,4},{  0,4},
	{440,4},{  0,4},{440,4},{440,4},
	{523,4},{  0,4},{466,4},{440,4},
	{392,4},{  0,4},{392,4},{932,4},
	{880,4},{932,4},{880,4},{932,4},
	{392,4},{  0,4},{392,4},{932,4},
	{880,4},{932,4},{880,4},{932,4},
	{392,4},{  0,4},{392,4},{587,4},
	{523,4},{  0,4},{466,4},{  0,4},
	{440,4},{  0,4},{440,4},{440,4},
	{523,4},{  0,4},{466,4},{440,4},
	{392,4},{  0,4},{392,4},{932,4},
	{880,4},{932,4},{880,4},{932,4},
	{392,4},{  0,4},{392,4},{932,4},
	{880,4},{932,4},{880,4},{932,4},
};

#endif /* APP_LIB_PWM_MELODY_H_ */
