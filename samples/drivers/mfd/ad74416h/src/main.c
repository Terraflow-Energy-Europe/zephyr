/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AD74416H sample: drives 5 V on channel B through the DAC API, then reads
 * channel A and the die temperature and supplies once a second.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/mfd/ad74416h.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device *const ad = DEVICE_DT_GET(DT_NODELABEL(ad74416h));
static const struct device *const dac = DEVICE_DT_GET(DT_NODELABEL(ad74416h_dac));

/* Diagnostic slots, as assigned in the overlay */
static const uint8_t diags[] = {AD74416H_DIAG_TEMP, AD74416H_DIAG_AVDD_HI, AD74416H_DIAG_AVCC,
				AD74416H_DIAG_DVCC};
static const char *const diag_names[] = {"temp", "AVDD_HI", "AVCC", "DVCC"};

int main(void)
{
	struct dac_channel_cfg dac_cfg = {.channel_id = 1, .resolution = 16};
	uint32_t results[8];
	uint16_t code;
	uint8_t range;
	int ret;

	if (!device_is_ready(ad) || !device_is_ready(dac)) {
		LOG_ERR("AD74416H not ready, check wiring and supplies");
		return 0;
	}

	ret = ad74416h_dac_uv_to_code(false, 5000000, &code);
	if (ret == 0) {
		ret = dac_channel_setup(dac, &dac_cfg);
	}
	if (ret == 0) {
		ret = dac_write_value(dac, 1, code);
	}
	if (ret != 0) {
		LOG_ERR("Setting channel B failed: %d", ret);
	}

	ret = ad74416h_adc_range_get(ad, 0, &range);
	if (ret != 0) {
		LOG_ERR("Reading channel A range failed: %d", ret);
		return 0;
	}

	while (true) {
		ret = ad74416h_adc_convert(ad, BIT(0), BIT_MASK(4), results, K_SECONDS(1));
		if (ret != 0) {
			LOG_ERR("Conversion failed: %d", ret);
		} else {
			LOG_INF("Channel A: %lld uV",
				ad74416h_adc_code_to_nv(range, results[0]) / 1000);
			for (int d = 0; d < ARRAY_SIZE(diags); d++) {
				LOG_INF("%s: %lld %s", diag_names[d],
					ad74416h_diag_code_to_value(diags[d], results[4 + d], 0),
					diags[d] == AD74416H_DIAG_TEMP ? "mdegC" : "uV");
			}
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
