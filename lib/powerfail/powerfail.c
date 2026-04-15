/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/powerfail.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_AURORA_POWERFAIL_SHUTDOWN)
#include <zephyr/sys/poweroff.h>
#endif /* CONFIG_AURORA_POWERFAIL_SHUTDOWN */

#if defined(CONFIG_DATA_LOGGER)
#include <aurora/lib/data_logger.h>
#endif /* CONFIG_DATA_LOGGER */

#if defined(CONFIG_AURORA_NOTIFY)
#include <aurora/lib/notify.h>
#endif /* CONFIG_AURORA_NOTIFY */

LOG_MODULE_REGISTER(powerfail, CONFIG_AURORA_POWERFAIL_LOG_LEVEL);

#if defined(CONFIG_DATA_LOGGER)
static inline void emergency_stop_data_logger(struct data_logger *logger,
					      void *argv)
{
	atomic_set(&logger->state->running, 0);
}

static inline void emergency_recover_data_logger(struct data_logger *logger,
						 void *argv)
{
	atomic_set(&logger->state->running, 1);
}
#endif /* CONFIG_DATA_LOGGER */

static const struct gpio_dt_spec pfail_pin =
    GPIO_DT_SPEC_GET(DT_CHOSEN(auxspace_pfm), gpios);

static struct gpio_callback pfail_cb_data;
static powerfail_cb_t pfail_assert_cb;
static powerfail_cb_t pfail_deassert_cb;

static atomic_t pfail_state = ATOMIC_INIT(0);

static inline void emergency_state_save(void)
{
#if defined(CONFIG_DATA_LOGGER)
	data_logger_foreach(emergency_stop_data_logger, NULL);
#endif /* CONFIG_DATA_LOGGER */

#if defined(CONFIG_AURORA_NOTIFY)
	notify_powerfail(0);
#endif /* CONFIG_AURORA_NOTIFY */
}

static inline void emergency_state_recover(void)
{
#if defined(CONFIG_DATA_LOGGER)
	data_logger_foreach(emergency_recover_data_logger, NULL);
#endif /* CONFIG_DATA_LOGGER */

#if defined(CONFIG_AURORA_NOTIFY)
	notify_powerfail(1);
#endif /* CONFIG_AURORA_NOTIFY */
}

static void powerfail_isr(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	int val = gpio_pin_get_dt(&pfail_pin);
	atomic_val_t prev = atomic_set(&pfail_state, val);

	/* Ignore repeated edges with the same value (bounce) */
	if (val == prev)
		return;

	if (val > 0) {
		/* Power failure asserted */
		emergency_state_save();

		if (pfail_assert_cb)
			pfail_assert_cb();

#if defined(CONFIG_AURORA_POWERFAIL_SHUTDOWN)
		sys_poweroff();
#endif /* CONFIG_AURORA_POWERFAIL_SHUTDOWN */
	} else {
		/* Power failure recovery */
		emergency_state_recover();

		/* Power restored (pin back to default pullup) */
		if (pfail_deassert_cb)
			pfail_deassert_cb();
	}
}

void powerfail_setup(powerfail_cb_t assert_cb, powerfail_cb_t deassert_cb)
{
	int ret;

	pfail_assert_cb = assert_cb;
	pfail_deassert_cb = deassert_cb;

	if (!gpio_is_ready_dt(&pfail_pin)) {
		LOG_ERR("powerfail GPIO device not ready");
		return;
	}

	ret = gpio_pin_configure_dt(&pfail_pin, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("failed to configure powerfail pin: %d", ret);
		return;
	}

	ret = gpio_pin_interrupt_configure_dt(&pfail_pin, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("failed to configure powerfail interrupt: %d", ret);
		return;
	}

	gpio_init_callback(&pfail_cb_data, powerfail_isr, BIT(pfail_pin.pin));
	ret = gpio_add_callback(pfail_pin.port, &pfail_cb_data);
	if (ret < 0) {
		LOG_ERR("failed to add powerfail callback: %d", ret);
		return;
	}

	LOG_INF("powerfail mitigation ready on %s pin %d",
		pfail_pin.port->name, pfail_pin.pin);
}
