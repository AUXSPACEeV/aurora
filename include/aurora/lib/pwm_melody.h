/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_PWM_MELODY_H_
#define APP_LIB_PWM_MELODY_H_

#include <inttypes.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

/**
 * @defgroup lib_pwm_melody PWM Music library
 * @ingroup lib
 * @{
 *
 * @brief PWM-based melody player for buzzer output.
 *
 * Plays a sequence of notes on a PWM-driven buzzer in a dedicated thread.
 * Each note is defined by a frequency and a duration divisor. A frequency
 * of 0 represents a rest (silence).
 */

/**
 * @brief A single note in a melody.
 */
struct pwm_melody_note {
	/** Frequency in Hz (0 for rest/silence). */
	uint16_t freq;
	/** Duration divisor — the note lasts for one beat divided by this value. */
	uint8_t  dur_div;
};

/**
 * @brief Runtime context for a melody player instance.
 */
struct pwm_melody_ctx {
	/** PWM device specification from devicetree. */
	const struct pwm_dt_spec *pwm;
	/** Pointer to the array of notes to play. */
	const struct pwm_melody_note *notes;
	/** Number of notes in the array. */
	size_t num_notes;
	/** Thread control block for the playback thread. */
	struct k_thread thread;
	/** Stack area for the playback thread. */
	k_thread_stack_t *stack;
	/** Size of the playback thread stack in bytes. */
	size_t stack_size;
	/** Flag indicating whether playback is in progress. */
	volatile bool playing;
};

/**
 * @brief Statically define and initialize a melody player context.
 *
 * This macro allocates the thread stack and initialises the context struct.
 *
 * @param _name       Variable name for the context.
 * @param _pwm        Pointer to a @c pwm_dt_spec for the buzzer output.
 * @param _notes      Array of @ref pwm_melody_note to play.
 * @param _stack_size Stack size in bytes for the playback thread.
 */
#define PWM_MELODY_CTX_DEFINE(_name, _pwm, _notes, _stack_size)                \
	static K_THREAD_STACK_DEFINE(_name##_stack, _stack_size);               \
	static struct pwm_melody_ctx _name = {                                  \
		.pwm = _pwm,                                                    \
		.notes = _notes,                                                \
		.num_notes = ARRAY_SIZE(_notes),                                \
		.stack = _name##_stack,                                         \
		.stack_size = _stack_size,                                      \
	}

/**
 * @brief Start playing a melody.
 *
 * Spawns a thread that iterates over the notes in @p ctx and drives the
 * PWM output accordingly.
 *
 * @param ctx Melody player context (must have been initialised, e.g. via
 *            @ref PWM_MELODY_CTX_DEFINE).
 * @return 0 on success, or a negative error code.
 */
int pwm_melody_start(struct pwm_melody_ctx *ctx);

/**
 * @brief Stop a melody that is currently playing.
 *
 * Signals the playback thread to stop and waits for it to terminate.
 * The PWM output is turned off before returning.
 *
 * @param ctx Melody player context.
 */
void pwm_melody_stop(struct pwm_melody_ctx *ctx);

/** Astronomia (Coffin Dance) melody. Ideal for post-flight celebration. */
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

/** @} */

#endif /* APP_LIB_PWM_MELODY_H_ */
