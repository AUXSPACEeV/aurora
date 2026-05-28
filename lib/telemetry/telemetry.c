/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

#include <aurora/lib/telemetry.h>

LOG_MODULE_REGISTER(telemetry, CONFIG_AURORA_TELEMETRY_LOG_LEVEL);

int telemetry_init(void)
{
	int rc = 0;

	STRUCT_SECTION_FOREACH(telemetry_backend, backend) {
		if (!backend->api || !backend->api->init) {
			continue;
		}
		int ret = backend->api->init();
		if (ret) {
			LOG_ERR("backend %s init failed (%d)", backend->name, ret);
			if (!rc) {
				rc = ret;
			}
		}
	}
	return rc;
}

int telemetry_send_sm_update(enum sm_state state,
			     const struct sm_inputs *inputs)
{
	int rc = 0;

	STRUCT_SECTION_FOREACH(telemetry_backend, backend) {
		if (!backend->api || !backend->api->send_sm_update) {
			continue;
		}
		int ret = backend->api->send_sm_update(state, inputs);
		if (ret && !rc) {
			rc = ret;
		}
	}
	return rc;
}
