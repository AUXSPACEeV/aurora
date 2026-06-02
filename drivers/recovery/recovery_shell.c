/*
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/drivers/pyro.h>
#include <zephyr/device.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>

#define ARGV_DEV     1
#define ARGV_CHANNEL 2

static const struct device *get_pyro_device(const struct shell *sh,
					    const char *name)
{
	const struct device *dev;

	dev = device_get_binding(name);
	if (dev == NULL) {
		shell_error(sh, "unknown pyro device: %s", name);
		return NULL;
	}

	if (!DEVICE_API_IS(pyro, dev)) {
		shell_error(sh, "%s is not a pyro device", name);
		return NULL;
	}

	return dev;
}

static int parse_common_args(const struct shell *sh, char **argv,
			     const struct device **dev, uint32_t *channel)
{
	int ret = 0;

	*dev = get_pyro_device(sh, argv[ARGV_DEV]);
	if (*dev == NULL) {
		return -ENODEV;
	}

	*channel = shell_strtoul(argv[ARGV_CHANNEL], 10, &ret);
	if (ret != 0) {
		shell_error(sh, "invalid channel: %s", argv[ARGV_CHANNEL]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_pyro_devices(const struct shell *sh, size_t argc, char **argv)
{
	bool found = false;

	STRUCT_SECTION_FOREACH(device, dev) {
		if (!device_is_ready(dev)) {
			continue;
		}
		if (!DEVICE_API_IS(pyro, dev)) {
			continue;
		}

		int nch = pyro_get_nchannels(dev);

		shell_print(sh, "%-20s (channels: %d)", dev->name,
			    nch >= 0 ? nch : 0);
		found = true;
	}

	if (!found) {
		shell_print(sh, "No pyro devices found");
	}

	return 0;
}

static int cmd_pyro_state(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;

	dev = get_pyro_device(sh, argv[ARGV_DEV]);
	if (dev == NULL) {
		return -ENODEV;
	}

	int nch = pyro_get_nchannels(dev);

	if (nch < 0) {
		shell_error(sh, "failed to get channel count: %d", nch);
		return nch;
	}

	shell_print(sh, "Device: %s (%d channels)", dev->name, nch);

	for (uint32_t ch = 0; ch < (uint32_t)nch; ch++) {
		uint32_t cap_mv = 0;
		uint32_t sense_mv = 0;
		int cap_ret, sense_ret;

		cap_ret = pyro_read_cap_channel(dev, ch, &cap_mv);
		sense_ret = pyro_sense_channel(dev, ch, &sense_mv);

		shell_fprintf(sh, SHELL_NORMAL, "  ch %u:", ch);
		if (cap_ret == 0) {
			shell_fprintf(sh, SHELL_NORMAL, " cap=%u mV", cap_mv);
		} else {
			shell_fprintf(sh, SHELL_NORMAL, " cap=n/a");
		}
		if (sense_ret == 0) {
			shell_fprintf(sh, SHELL_NORMAL, " sense=%u mV", sense_mv);
		} else {
			shell_fprintf(sh, SHELL_NORMAL, " sense=n/a");
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}

	return 0;
}

static int cmd_pyro_channels(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;

	dev = get_pyro_device(sh, argv[ARGV_DEV]);
	if (dev == NULL) {
		return -ENODEV;
	}

	int nch = pyro_get_nchannels(dev);

	if (nch < 0) {
		shell_error(sh, "failed to get channel count: %d", nch);
		return nch;
	}

	shell_print(sh, "Device: %s", dev->name);
	shell_print(sh, "Channels: %d", nch);
	for (uint32_t ch = 0; ch < (uint32_t)nch; ch++) {
		shell_print(sh, "  ch %u", ch);
	}

	return 0;
}

static int cmd_pyro_arm(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t channel;
	int ret;

	ret = parse_common_args(sh, argv, &dev, &channel);
	if (ret != 0) {
		return ret;
	}

	ret = pyro_arm(dev, channel);
	if (ret != 0) {
		shell_error(sh, "failed to arm ch %u: %d", channel, ret);
		return ret;
	}

	shell_print(sh, "%s ch %u armed", dev->name, channel);
	return 0;
}

static int cmd_pyro_disarm(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t channel;
	int ret;

	ret = parse_common_args(sh, argv, &dev, &channel);
	if (ret != 0) {
		return ret;
	}

	ret = pyro_disarm(dev, channel);
	if (ret != 0) {
		shell_error(sh, "failed to disarm ch %u: %d", channel, ret);
		return ret;
	}

	shell_print(sh, "%s ch %u disarmed", dev->name, channel);
	return 0;
}

static int cmd_pyro_trigger(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t channel;
	int ret;

	ret = parse_common_args(sh, argv, &dev, &channel);
	if (ret != 0) {
		return ret;
	}

	ret = pyro_trigger_channel(dev, channel);
	if (ret != 0) {
		shell_error(sh, "failed to trigger ch %u: %d", channel, ret);
		return ret;
	}

	shell_print(sh, "%s ch %u triggered", dev->name, channel);
	return 0;
}

static int cmd_pyro_secure(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t channel;
	int ret;

	ret = parse_common_args(sh, argv, &dev, &channel);
	if (ret != 0) {
		return ret;
	}

	ret = pyro_secure(dev, channel);
	if (ret != 0) {
		shell_error(sh, "failed to secure ch %u: %d", channel, ret);
		return ret;
	}

	shell_print(sh, "%s ch %u secured (shorted)", dev->name, channel);
	return 0;
}

static int cmd_pyro_sense(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t channel;
	uint32_t val;
	int ret;

	ret = parse_common_args(sh, argv, &dev, &channel);
	if (ret != 0) {
		return ret;
	}

	ret = pyro_sense_channel(dev, channel, &val);
	if (ret != 0) {
		shell_error(sh, "failed to read sense ch %u: %d", channel, ret);
		return ret;
	}

	shell_print(sh, "%s ch %u sense: %u mV", dev->name, channel, val);
	return 0;
}

static int cmd_pyro_cap(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t channel;
	uint32_t val;
	int ret;

	ret = parse_common_args(sh, argv, &dev, &channel);
	if (ret != 0) {
		return ret;
	}

	ret = pyro_read_cap_channel(dev, channel, &val);
	if (ret != 0) {
		shell_error(sh, "failed to read cap ch %u: %d", channel, ret);
		return ret;
	}

	shell_print(sh, "%s ch %u cap: %u mV", dev->name, channel, val);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_pyro,
	SHELL_CMD(devices, NULL,
		  "List all pyro devices",
		  cmd_pyro_devices),
	SHELL_CMD_ARG(state, NULL,
		      "Query pyro device state\n"
		      "Usage: pyro state <device>",
		      cmd_pyro_state, 2, 0),
	SHELL_CMD_ARG(channels, NULL,
		      "List channels of a pyro device\n"
		      "Usage: pyro channels <device>",
		      cmd_pyro_channels, 2, 0),
	SHELL_CMD_ARG(arm, NULL,
		      "Arm a pyro channel\n"
		      "Usage: pyro arm <device> <channel>",
		      cmd_pyro_arm, 3, 0),
	SHELL_CMD_ARG(disarm, NULL,
		      "Disarm a pyro channel\n"
		      "Usage: pyro disarm <device> <channel>",
		      cmd_pyro_disarm, 3, 0),
	SHELL_CMD_ARG(trigger, NULL,
		      "Trigger (fire) a pyro channel\n"
		      "Usage: pyro trigger <device> <channel>",
		      cmd_pyro_trigger, 3, 0),
	SHELL_CMD_ARG(secure, NULL,
		      "Secure (short) a pyro channel\n"
		      "Usage: pyro secure <device> <channel>",
		      cmd_pyro_secure, 3, 0),
	SHELL_CMD_ARG(sense, NULL,
		      "Read sense ADC of a pyro channel\n"
		      "Usage: pyro sense <device> <channel>",
		      cmd_pyro_sense, 3, 0),
	SHELL_CMD_ARG(cap, NULL,
		      "Read capacitor voltage of a pyro channel\n"
		      "Usage: pyro cap <device> <channel>",
		      cmd_pyro_cap, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(pyro, &sub_pyro, "Pyro device commands", NULL);
