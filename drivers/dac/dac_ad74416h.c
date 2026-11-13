/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AD74416H DAC: Zephyr DAC API on top of the core driver. DAC channels 0..3
 * are I/O channels A..D, 16-bit. What the code means (voltage, current, or a
 * current limit) depends on the channel function; see
 * ad74416h_dac_uv_to_code() and ad74416h_dac_ua_to_code().
 */

#define DT_DRV_COMPAT adi_ad74416h_dac

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/mfd/ad74416h.h>

LOG_MODULE_REGISTER(dac_ad74416h, CONFIG_DAC_LOG_LEVEL);

BUILD_ASSERT(CONFIG_DAC_AD74416H_INIT_PRIORITY > CONFIG_MFD_INIT_PRIORITY,
	     "AD74416H DAC must init after the MFD core");

#define AD74416H_DAC_RESOLUTION 16

struct ad74416h_dac_config {
	const struct device *parent;
};

/* Channel functions that drive the DAC */
static bool func_uses_dac(uint8_t func)
{
	switch (func) {
	case AD74416H_CH_FUNC_VOUT:
	case AD74416H_CH_FUNC_IOUT:
	case AD74416H_CH_FUNC_IOUT_HART:
	case AD74416H_CH_FUNC_IIN_LOOP_PWR:
	case AD74416H_CH_FUNC_IIN_LOOP_PWR_HART:
	case AD74416H_CH_FUNC_DIN_LOOP:
		return true;
	default:
		return false;
	}
}

static int ad74416h_dac_channel_setup(const struct device *dev, const struct dac_channel_cfg *cfg)
{
	const struct ad74416h_dac_config *config = dev->config;
	uint8_t func;
	int ret;

	if (cfg->channel_id >= AD74416H_NUM_CHANNELS ||
	    cfg->resolution != AD74416H_DAC_RESOLUTION) {
		return -EINVAL;
	}

	if (cfg->buffered || cfg->internal) {
		return -ENOTSUP;
	}

	ret = ad74416h_channel_func_get(config->parent, cfg->channel_id, &func);
	if (ret != 0) {
		return ret;
	}

	if (!func_uses_dac(func)) {
		LOG_WRN("Channel %u: function %u does not use the DAC", cfg->channel_id, func);
	}

	return 0;
}

static int ad74416h_dac_write_value(const struct device *dev, uint8_t channel, uint32_t value)
{
	const struct ad74416h_dac_config *config = dev->config;

	if (channel >= AD74416H_NUM_CHANNELS || value > UINT16_MAX) {
		return -EINVAL;
	}

	return ad74416h_dac_set(config->parent, channel, value);
}

static int ad74416h_dac_init(const struct device *dev)
{
	const struct ad74416h_dac_config *config = dev->config;

	return device_is_ready(config->parent) ? 0 : -ENODEV;
}

static DEVICE_API(dac, ad74416h_dac_api) = {
	.channel_setup = ad74416h_dac_channel_setup,
	.write_value = ad74416h_dac_write_value,
};

#define AD74416H_DAC_INIT(n)                                                                       \
	static const struct ad74416h_dac_config ad74416h_dac_config_##n = {                        \
		.parent = DEVICE_DT_GET(DT_INST_PARENT(n)),                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, ad74416h_dac_init, NULL, NULL, &ad74416h_dac_config_##n,          \
			      POST_KERNEL, CONFIG_DAC_AD74416H_INIT_PRIORITY, &ad74416h_dac_api);

DT_INST_FOREACH_STATUS_OKAY(AD74416H_DAC_INIT)
