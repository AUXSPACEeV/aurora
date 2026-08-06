/**
 * @file state_rbf.c
 * @brief Remove-before-flight pin as the state machine's arm input.
 *
 * Reads the mechanical key or shorting plug that is pulled off the rocket on
 * the pad, from the GPIO behind the "auxspace,rbf" chosen node, and hands the
 * debounced value to the common core, which substitutes it for
 * @c sm_inputs.armed on every update.
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "state_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(state_machine, CONFIG_STATE_MACHINE_LOG_LEVEL);

/* Build-time dependency */
#if !DT_HAS_CHOSEN(auxspace_rbf)
#error "CONFIG_AURORA_STATE_MACHINE_RBF requires DT chosen 'auxspace,rbf' to point at a node with a gpios property (the remove-before-flight input)."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_rbf), okay),
	     "the 'auxspace,rbf' chosen node must have status \"okay\"");

static const struct gpio_dt_spec rbf_pin =
    GPIO_DT_SPEC_GET(DT_CHOSEN(auxspace_rbf), gpios);

static struct gpio_callback rbf_cb_data;

/* Non-zero while the interlock is out, i.e. while the vehicle is armed.
 * Starts safe so a failed init can never report "armed". */
static atomic_t rbf_armed = ATOMIC_INIT(0);

static struct k_work_delayable rbf_debounce_work;

/** @brief Translate the raw pin level into an arm verdict. */
static inline atomic_val_t rbf_armed_from_level(int level)
{
	/* The line is asserted while the interlock is installed, which is the
	 * *safe* state; pulling it arms the vehicle. */
	return (level > 0) ? 0 : 1;
}

/**
 * @brief Sample the line once the contact is stable.
 *
 * Runs on the system workqueue; the state machine picks the result up on its
 * next update rather than being driven from here.
 */
static void rbf_debounce_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int level = gpio_pin_get_dt(&rbf_pin);
	if (level < 0) {
		LOG_ERR("failed to read RBF pin: %d", level);
		return;
	}

	atomic_val_t armed = rbf_armed_from_level(level);
	atomic_val_t prev = atomic_set(&rbf_armed, armed);

	/* The contact bounced back to where it started */
	if (armed == prev) {
		LOG_DBG("edge on %s pin %d, RBF still %s", rbf_pin.port->name,
			rbf_pin.pin, armed ? "removed" : "installed");
		return;
	}

	LOG_INF("RBF %s: vehicle %s", armed ? "removed" : "installed",
		armed ? "ARMED" : "SAFE");
	sm_event(armed ? "RBF removed" : "RBF installed");
}

static void rbf_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Restart the window on every edge: the level is only sampled once the
	 * contact has been quiet for a full debounce period. */
	k_work_reschedule(&rbf_debounce_work,
			  K_MSEC(CONFIG_AURORA_STATE_MACHINE_RBF_DEBOUNCE_MS));
}

/* sm_rbf_init – see state_internal.h */
int sm_rbf_init(void)
{
	static bool wired;
	int ret, level;

	if (!gpio_is_ready_dt(&rbf_pin)) {
		LOG_ERR("RBF GPIO device not ready");
		return -ENODEV;
	}

	/* does not run on the first sm_init. */
	if (wired) {
		level = gpio_pin_get_dt(&rbf_pin);
		if (level < 0) {
			LOG_ERR("failed to read RBF pin: %d", level);
			return level;
		}

		atomic_set(&rbf_armed, rbf_armed_from_level(level));
		return 0;
	}

	ret = gpio_pin_configure_dt(&rbf_pin, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("failed to configure RBF pin: %d", ret);
		return ret;
	}

	k_work_init_delayable(&rbf_debounce_work, rbf_debounce_handler);

	ret = gpio_pin_interrupt_configure_dt(&rbf_pin, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("failed to configure RBF interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&rbf_cb_data, rbf_isr, BIT(rbf_pin.pin));
	ret = gpio_add_callback(rbf_pin.port, &rbf_cb_data);
	if (ret < 0) {
		LOG_ERR("failed to add RBF callback: %d", ret);
		return ret;
	}

	wired = true;

	/* This should always print "SAFE", since the RBF is not latched on
	 * boot, so that forgetting to insert the RBF-Plug does not result in
	 * the sm arming.
	 */
	LOG_INF("RBF arm input ready on %s pin %d, vehicle %s",
		rbf_pin.port->name, rbf_pin.pin,
		sm_rbf_armed() ? "ARMED" : "SAFE");

	return 0;
}

/* sm_rbf_armed – see state_internal.h */
bool sm_rbf_armed(void)
{
	return atomic_get(&rbf_armed) != 0;
}
