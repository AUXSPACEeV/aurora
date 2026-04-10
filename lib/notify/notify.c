/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/notify.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

LOG_MODULE_REGISTER(notify, CONFIG_AURORA_NOTIFY_LOG_LEVEL);

int notify_init(void)
{
	int rc = 0;

	STRUCT_SECTION_FOREACH(notify_backend, backend) {
		if (!backend->api) {
			LOG_ERR("Backend with NULL api pointer: %p", backend);
			continue;
		}
		if (backend->api->init) {
			int ret = backend->api->init();

			if (ret) {
				LOG_ERR("notify backend init failed (%d)", ret);
				if (!rc) {
					rc = ret;
				}
			}
		}
	}
	return rc;
}

int notify_boot(void)
{
	int rc = 0;

	STRUCT_SECTION_FOREACH(notify_backend, backend) {
		if (!backend->api) {
			LOG_ERR("Backend with NULL api pointer: %p", backend);
			continue;
		}
		if (backend->api->on_boot) {
			int ret = backend->api->on_boot();

			if (ret) {
				LOG_ERR("notify on_boot failed (%d)", ret);
				if (!rc) {
					rc = ret;
				}
			}
		}
	}
	return rc;
}

int notify_state_change(enum sm_state prev, enum sm_state next)
{
	int rc = 0;

	STRUCT_SECTION_FOREACH(notify_backend, backend) {
		if (!backend->api) {
			LOG_ERR("Backend with NULL api pointer: %p", backend);
			continue;
		}
		if (backend->api->on_state_change) {
			int ret = backend->api->on_state_change(prev, next);

			if (ret) {
				LOG_ERR("notify on_state_change failed (%d)", ret);
				if (!rc) {
					rc = ret;
				}
			}
		}
	}
	return rc;
}

int notify_error(void)
{
	int rc = 0;

	STRUCT_SECTION_FOREACH(notify_backend, backend) {
		if (!backend->api) {
			LOG_ERR("Backend with NULL api pointer: %p", backend);
			continue;
		}
		if (backend->api->on_error) {
			int ret = backend->api->on_error();

			if (ret) {
				LOG_ERR("notify on_error failed (%d)", ret);
				if (!rc) {
					rc = ret;
				}
			}
		}
	}
	return rc;
}
