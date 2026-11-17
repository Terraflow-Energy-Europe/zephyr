/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AD74416H driver tests against the register-level emulator.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/drivers/mfd/ad74416h.h>
#include "mfd_ad74416h_emul.h"

#define AD_NODE DT_NODELABEL(ad74416h)

static const struct device *const dev = DEVICE_DT_GET(AD_NODE);
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(ad74416h_adc));
static const struct device *const dac = DEVICE_DT_GET(DT_NODELABEL(ad74416h_dac));
static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(ad74416h_gpio));
static const struct emul *const emul = EMUL_DT_GET(AD_NODE);

static const struct adc_dt_spec adc_ch0 = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static const struct adc_dt_spec adc_ch1 = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1);
static const struct adc_dt_spec adc_diag0 = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2);
static const struct adc_dt_spec adc_diag1 = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 3);

static uint16_t reg(uint8_t r)
{
	return ad74416h_emul_reg_get(emul, r);
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);

	ad74416h_emul_adc_hang(emul, false);
	ad74416h_emul_cal_mem_fault(emul, false);
	(void)ad74416h_adc_stop(dev, false);
	(void)ad74416h_alert_callback_set(dev, NULL, NULL);
	zassert_ok(ad74416h_reset(dev));
	ad74416h_emul_stats_clear(emul);
}

static void after_each(void *fixture)
{
	const struct ad74416h_emul_stats *s = ad74416h_emul_stats_get(emul);

	ARG_UNUSED(fixture);

	zassert_equal(s->bad_frames, 0, "driver sent %u bad frames", s->bad_frames);
	zassert_equal(s->violations, 0, "%u data sheet sequence violations", s->violations);
}

ZTEST_SUITE(ad74416h, NULL, NULL, before_each, after_each, NULL);

ZTEST(ad74416h, test_devices_ready)
{
	zassert_true(device_is_ready(dev));
	zassert_true(device_is_ready(adc));
	zassert_true(device_is_ready(dac));
	zassert_true(device_is_ready(gpio));
}

ZTEST(ad74416h, test_id)
{
	uint8_t rev;
	uint32_t uid;

	zassert_ok(ad74416h_get_id(dev, &rev, &uid));
	zassert_equal(rev, 2);
	/* SILICON_ID0 = 0x55, SILICON_ID1 = 0xABC */
	zassert_equal(uid, 0x55 | (0x3C << 7) | (0x2A << 13));
}

ZTEST(ad74416h, test_reset_uses_keys_and_clears_alerts)
{
	ad74416h_emul_stats_clear(emul);
	zassert_ok(ad74416h_reset(dev));

	zassert_equal(ad74416h_emul_stats_get(emul)->resets, 1);
	zassert_equal(reg(AD74416H_REG_ALERT_STATUS), 0, "alerts not cleared after reset");
	zassert_true(reg(AD74416H_REG_PWR_OPTIM_CONFIG) & AD74416H_PWR_REF_EN,
		     "internal reference not enabled after reset");
}

ZTEST(ad74416h, test_cal_mem_fault)
{
	ad74416h_emul_cal_mem_fault(emul, true);
	zassert_equal(ad74416h_reset(dev), -EIO);

	ad74416h_emul_cal_mem_fault(emul, false);
	zassert_ok(ad74416h_reset(dev));
}

ZTEST(ad74416h, test_register_access)
{
	static const uint8_t regs[] = {AD74416H_REG_SCRATCH(0), AD74416H_REG_SCRATCH(1),
				       AD74416H_REG_SCRATCH(2), AD74416H_REG_SCRATCH(3)};
	uint16_t vals[ARRAY_SIZE(regs)];

	for (int i = 0; i < ARRAY_SIZE(regs); i++) {
		zassert_ok(ad74416h_reg_write(dev, regs[i], 0x1111 * (i + 1)));
	}

	zassert_ok(ad74416h_reg_read_multi(dev, regs, vals, ARRAY_SIZE(regs)));
	for (int i = 0; i < ARRAY_SIZE(regs); i++) {
		zassert_equal(vals[i], 0x1111 * (i + 1));
	}

	zassert_ok(ad74416h_reg_update(dev, AD74416H_REG_SCRATCH(0), 0x00F0, 0x0050));
	zassert_ok(ad74416h_reg_read(dev, AD74416H_REG_SCRATCH(0), &vals[0]));
	zassert_equal(vals[0], 0x1151);
}

ZTEST(ad74416h, test_devicetree_config)
{
	/* Channel 0: bipolar voltage output, starting at 0 V */
	zassert_equal(reg(AD74416H_REG_CH_FUNC_SETUP(0)), AD74416H_CH_FUNC_VOUT);
	zassert_true(reg(AD74416H_REG_OUTPUT_CONFIG(0)) & AD74416H_OUT_VOUT_RANGE);
	zassert_equal(reg(AD74416H_REG_DAC_CODE(0)), 0x8000);

	/* Channel 1: RTD, 500 uA, 1 V reference, 0 V to 12 V range */
	zassert_equal(reg(AD74416H_REG_CH_FUNC_SETUP(1)), AD74416H_CH_FUNC_RES_MEAS);
	zassert_equal(reg(AD74416H_REG_RTD_CONFIG(1)), AD74416H_RTD_ADC_REF);
	zassert_equal(FIELD_GET(AD74416H_ADC_CONV_RANGE, reg(AD74416H_REG_ADC_CONFIG(1))),
		      AD74416H_ADC_RANGE_0_12V);
	zassert_equal(FIELD_GET(AD74416H_ADC_CONV_MUX, reg(AD74416H_REG_ADC_CONFIG(1))),
		      AD74416H_ADC_MUX_LF_TO_VSENSEN, "function default input was lost");

	/* Channel 2: IEC 61131-2 type 1/3 digital input with counter */
	zassert_equal(reg(AD74416H_REG_CH_FUNC_SETUP(2)), AD74416H_CH_FUNC_DIN_LOGIC);
	zassert_equal(reg(AD74416H_REG_DIN_CONFIG1(2)), AD74416H_DIN_THRESH_MODE | 0x37);
	zassert_equal(reg(AD74416H_REG_DIN_CONFIG0(2)),
		      AD74416H_DIN_COUNT_EN | AD74416H_DIN_COMPARATOR_EN |
			      FIELD_PREP(AD74416H_DIN_SINK, 0x14) |
			      FIELD_PREP(AD74416H_DIN_DEBOUNCE_TIME, 0xB));

	/* Channel 3: high impedance with the digital output enabled, FET off */
	zassert_equal(reg(AD74416H_REG_CH_FUNC_SETUP(3)), AD74416H_CH_FUNC_HIGH_IMP);
	zassert_equal(reg(AD74416H_REG_DO_EXT_CONFIG(3)), AD74416H_DO_MODE |
								  FIELD_PREP(AD74416H_DO_T1, 5) |
								  FIELD_PREP(AD74416H_DO_T2, 0x17));

	/* Diagnostics: TEMP, AVDD_HI, AVCC, DVCC */
	zassert_equal(reg(AD74416H_REG_DIAG_ASSIGN), 0x2351);

	/* adi,internal-ref */
	zassert_equal(reg(AD74416H_REG_PWR_OPTIM_CONFIG), AD74416H_PWR_REF_EN);
}

ZTEST(ad74416h, test_function_switch_sequence)
{
	const struct ad74416h_emul_stats *s = ad74416h_emul_stats_get(emul);
	struct ad74416h_channel_config cfg;
	int64_t start;

	ad74416h_channel_config_init(&cfg);
	cfg.func = AD74416H_CH_FUNC_IOUT_HART;
	cfg.dac_code = 0x1234;

	start = k_uptime_get();
	zassert_ok(ad74416h_channel_configure(dev, 0, &cfg));

	/* VOUT -> HIGH_IMP -> IOUT_HART, never directly */
	zassert_equal(s->func_history_len[0], 2);
	zassert_equal(s->func_history[0][0], AD74416H_CH_FUNC_HIGH_IMP);
	zassert_equal(s->func_history[0][1], AD74416H_CH_FUNC_IOUT_HART);

	/* 300 us before and 4.2 ms after the function change */
	zassert_true(k_uptime_get() - start >= 4, "channel init time not respected");

	/* The bipolar range was dropped in high impedance, the DAC code kept */
	zassert_false(reg(AD74416H_REG_OUTPUT_CONFIG(0)) & AD74416H_OUT_VOUT_RANGE);
	zassert_equal(reg(AD74416H_REG_DAC_CODE(0)), 0x1234);
	/* Function default: track supply */
	zassert_equal(FIELD_GET(AD74416H_OUT_AVDD_SELECT, reg(AD74416H_REG_OUTPUT_CONFIG(0))),
		      AD74416H_AVDD_TRACK);
	zassert_equal(reg(AD74416H_REG_CHANNEL_ALERT_STATUS(0)), 0, "VIOUT shut down");

	/* Config read back */
	zassert_ok(ad74416h_channel_config_get(dev, 0, &cfg));
	zassert_equal(cfg.func, AD74416H_CH_FUNC_IOUT_HART);
}

ZTEST(ad74416h, test_function_switch_disables_do)
{
	struct ad74416h_channel_config cfg;

	zassert_ok(ad74416h_do_set(dev, 3, true));
	zassert_true(reg(AD74416H_REG_DO_EXT_CONFIG(3)) & AD74416H_DO_DATA);

	ad74416h_channel_config_init(&cfg);
	cfg.func = AD74416H_CH_FUNC_VIN;
	zassert_ok(ad74416h_channel_configure(dev, 3, &cfg));

	zassert_false(reg(AD74416H_REG_DO_EXT_CONFIG(3)) & (AD74416H_DO_DATA | AD74416H_DO_MODE));
	zassert_equal(reg(AD74416H_REG_CH_FUNC_SETUP(3)), AD74416H_CH_FUNC_VIN);
}

ZTEST(ad74416h, test_invalid_config)
{
	struct ad74416h_channel_config cfg;

	ad74416h_channel_config_init(&cfg);
	zassert_equal(ad74416h_channel_configure(dev, 4, &cfg), -EINVAL);

	cfg.func = 0x6;
	zassert_equal(ad74416h_channel_configure(dev, 0, &cfg), -EINVAL);

	ad74416h_channel_config_init(&cfg);
	cfg.din_threshold = 99;
	zassert_equal(ad74416h_channel_configure(dev, 0, &cfg), -EINVAL);
}

ZTEST(ad74416h, test_adc_api)
{
	uint32_t buf32[3];
	uint16_t buf16[2];
	struct adc_sequence seq = {
		.buffer = buf32,
		.buffer_size = sizeof(buf32),
	};

	zassert_ok(adc_channel_setup_dt(&adc_ch0));
	zassert_ok(adc_channel_setup_dt(&adc_ch1));
	zassert_ok(adc_channel_setup_dt(&adc_diag0));
	zassert_ok(adc_channel_setup_dt(&adc_diag1));

	ad74416h_emul_adc_code_set(emul, 0, 0x123456);
	ad74416h_emul_adc_code_set(emul, 1, 0xABCDEF);
	ad74416h_emul_adc_code_set(emul, 4, 0x0A5A);
	ad74416h_emul_adc_code_set(emul, 5, 0x8000);

	seq.channels = BIT(0) | BIT(1) | BIT(4);
	seq.resolution = 24;
	zassert_ok(adc_read(adc, &seq));
	zassert_equal(buf32[0], 0x123456);
	zassert_equal(buf32[1], 0xABCDEF);
	zassert_equal(buf32[2], 0x0A5A);

	/* Only the requested channels were enabled */
	zassert_equal(reg(AD74416H_REG_ADC_CONV_CTRL) & 0xFF, BIT(0) | BIT(1) | BIT(4));

	/* 16-bit samples for diagnostics only */
	seq.buffer = buf16;
	seq.buffer_size = sizeof(buf16);
	seq.channels = BIT(4) | BIT(5);
	seq.resolution = 16;
	zassert_ok(adc_read(adc, &seq));
	zassert_equal(buf16[0], 0x0A5A);
	zassert_equal(buf16[1], 0x8000);

	seq.channels = BIT(0);
	zassert_equal(adc_read(adc, &seq), -EINVAL, "16-bit read of a 24-bit channel");

	seq.channels = BIT(3); /* not set up */
	seq.resolution = 24;
	seq.buffer = buf32;
	seq.buffer_size = sizeof(buf32);
	zassert_equal(adc_read(adc, &seq), -EINVAL);

	seq.channels = BIT(0) | BIT(1);
	seq.buffer_size = sizeof(uint32_t);
	zassert_equal(adc_read(adc, &seq), -ENOMEM);
}

ZTEST(ad74416h, test_adc_extra_samplings)
{
	uint32_t buf[3];
	struct adc_sequence_options opts = {.extra_samplings = 2};
	struct adc_sequence seq = {
		.options = &opts,
		.channels = BIT(0),
		.buffer = buf,
		.buffer_size = sizeof(buf),
		.resolution = 24,
	};

	zassert_ok(adc_channel_setup_dt(&adc_ch0));
	ad74416h_emul_adc_code_set(emul, 0, 0x000777);
	zassert_ok(adc_read(adc, &seq));
	for (int i = 0; i < ARRAY_SIZE(buf); i++) {
		zassert_equal(buf[i], 0x000777);
	}
}

ZTEST(ad74416h, test_adc_bad_channel_cfg)
{
	struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_2,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = 0,
	};

	zassert_equal(adc_channel_setup(adc, &cfg), -ENOTSUP);
	cfg.gain = ADC_GAIN_1;
	cfg.channel_id = 8;
	zassert_equal(adc_channel_setup(adc, &cfg), -EINVAL);
}

ZTEST(ad74416h, test_adc_timeout)
{
	uint32_t results[8];

	ad74416h_emul_adc_hang(emul, true);
	zassert_equal(ad74416h_adc_convert(dev, BIT(0), 0, results, K_MSEC(20)), -ETIMEDOUT);
	ad74416h_emul_adc_hang(emul, false);
	zassert_ok(ad74416h_adc_convert(dev, BIT(0), 0, results, K_MSEC(20)));
}

ZTEST(ad74416h, test_adc_continuous)
{
	uint32_t results[8];
	uint8_t seq_count;

	ad74416h_emul_adc_code_set(emul, 2, 0x00ABCD);
	zassert_ok(ad74416h_adc_start_continuous(dev, BIT(2), BIT(0)));
	zassert_equal(FIELD_GET(AD74416H_CONV_SEQ, reg(AD74416H_REG_ADC_CONV_CTRL)),
		      AD74416H_CONV_SEQ_CONTINUOUS);

	/* Configuration changes and single sequences are refused meanwhile */
	zassert_equal(ad74416h_adc_convert(dev, BIT(0), 0, results, K_MSEC(20)), -EBUSY);
	zassert_equal(ad74416h_adc_channel_set(dev, 0, AD74416H_ADC_RANGE_PM_12V,
					       AD74416H_ADC_RATE_20SPS,
					       AD74416H_ADC_MUX_LF_TO_AGND),
		      -EBUSY);

	zassert_ok(ad74416h_adc_results_read(dev, BIT(2), 0, results, &seq_count));
	zassert_equal(results[2], 0x00ABCD);
	zassert_equal(seq_count, 1);

	zassert_ok(ad74416h_adc_stop(dev, true));
	zassert_equal(FIELD_GET(AD74416H_CONV_SEQ, reg(AD74416H_REG_ADC_CONV_CTRL)),
		      AD74416H_CONV_SEQ_POWER_DOWN);
	zassert_ok(ad74416h_adc_channel_set(dev, 0, AD74416H_ADC_RANGE_PM_12V,
					    AD74416H_ADC_RATE_4K8SPS, AD74416H_ADC_MUX_LF_TO_AGND));
	zassert_equal(reg(AD74416H_REG_ADC_CONFIG(0)),
		      FIELD_PREP(AD74416H_ADC_CONV_RATE, AD74416H_ADC_RATE_4K8SPS) |
			      FIELD_PREP(AD74416H_ADC_CONV_RANGE, AD74416H_ADC_RANGE_PM_12V));
}

ZTEST(ad74416h, test_diag_assign)
{
	uint8_t diag;

	zassert_ok(ad74416h_diag_assign(dev, 2, AD74416H_DIAG_I_DO_SRC));
	zassert_ok(ad74416h_diag_get(dev, 2, &diag));
	zassert_equal(diag, AD74416H_DIAG_I_DO_SRC);
	zassert_equal(reg(AD74416H_REG_DIAG_ASSIGN), 0x2C51);
	zassert_equal(ad74416h_diag_assign(dev, 4, 0), -EINVAL);

	zassert_ok(ad74416h_diag_rate_set(dev, AD74416H_DIAG_RATE_19K2SPS));
	zassert_ok(ad74416h_adc_convert(dev, 0, BIT(2), (uint32_t[8]){0}, K_MSEC(20)));
	zassert_equal(FIELD_GET(AD74416H_CONV_RATE_DIAG, reg(AD74416H_REG_ADC_CONV_CTRL)),
		      AD74416H_DIAG_RATE_19K2SPS);
}

ZTEST(ad74416h, test_dac_api)
{
	struct dac_channel_cfg cfg = {.channel_id = 0, .resolution = 16};

	zassert_ok(dac_channel_setup(dac, &cfg));
	zassert_ok(dac_write_value(dac, 0, 0x9000));
	zassert_equal(reg(AD74416H_REG_DAC_CODE(0)), 0x9000);
	zassert_equal(reg(AD74416H_REG_DAC_ACTIVE(0)), 0x9000);

	zassert_equal(dac_write_value(dac, 0, 0x10000), -EINVAL);
	cfg.resolution = 12;
	zassert_equal(dac_channel_setup(dac, &cfg), -EINVAL);
}

ZTEST(ad74416h, test_dac_linear_slew_busy)
{
	uint16_t active;

	zassert_ok(ad74416h_dac_slew_set(dev, 0, AD74416H_SLEW_LINEAR, AD74416H_SLEW_STEP_6P1,
					 AD74416H_SLEW_RATE_76K8HZ));
	zassert_equal(FIELD_GET(AD74416H_OUT_SLEW_EN, reg(AD74416H_REG_OUTPUT_CONFIG(0))),
		      AD74416H_SLEW_LINEAR);

	zassert_ok(ad74416h_dac_set(dev, 0, 0xA000));
	/* Still slewing towards 0xA000 */
	zassert_equal(ad74416h_dac_set(dev, 0, 0xB000), -EBUSY);

	ad74416h_emul_dac_settle(emul, 0);
	zassert_ok(ad74416h_dac_active_get(dev, 0, &active));
	zassert_equal(active, 0xA000);
	zassert_ok(ad74416h_dac_set(dev, 0, 0xB000));
}

ZTEST(ad74416h, test_ldac)
{
	zassert_ok(ad74416h_reg_update(dev, AD74416H_REG_OUTPUT_CONFIG(0),
				       AD74416H_OUT_WAIT_LDAC_CMD, AD74416H_OUT_WAIT_LDAC_CMD));
	zassert_ok(ad74416h_dac_set(dev, 0, 0x4000));
	zassert_equal(reg(AD74416H_REG_DAC_ACTIVE(0)), 0x8000);
	zassert_ok(ad74416h_dac_update_all(dev));
	zassert_equal(reg(AD74416H_REG_DAC_ACTIVE(0)), 0x4000);
}

ZTEST(ad74416h, test_gpio_pins)
{
	zassert_ok(gpio_pin_configure(gpio, AD74416H_GPIO_A, GPIO_OUTPUT_HIGH));
	zassert_equal(FIELD_GET(AD74416H_GPIO_SELECT, reg(AD74416H_REG_GPIO_CONFIG(0))),
		      AD74416H_GPIO_SEL_GPO);
	zassert_true(reg(AD74416H_REG_GPIO_CONFIG(0)) & AD74416H_GPIO_GPO_DATA);
	zassert_false(reg(AD74416H_REG_GPIO_CONFIG(0)) & AD74416H_GPIO_GP_WK_PD_EN);
	zassert_equal(gpio_pin_get(gpio, AD74416H_GPIO_A), 1);

	zassert_ok(gpio_pin_toggle(gpio, AD74416H_GPIO_A));
	zassert_equal(gpio_pin_get(gpio, AD74416H_GPIO_A), 0);

	zassert_ok(gpio_pin_configure(gpio, AD74416H_GPIO_F, GPIO_INPUT | GPIO_PULL_DOWN));
	zassert_equal(FIELD_GET(AD74416H_GPIO_SELECT, reg(AD74416H_REG_GPIO_CONFIG(5))),
		      AD74416H_GPIO_SEL_GPI);
	zassert_true(reg(AD74416H_REG_GPIO_CONFIG(5)) & AD74416H_GPIO_GP_WK_PD_EN);
	ad74416h_emul_gpi_set(emul, 5, true);
	zassert_equal(gpio_pin_get(gpio, AD74416H_GPIO_F), 1);
	ad74416h_emul_gpi_set(emul, 5, false);
	zassert_equal(gpio_pin_get(gpio, AD74416H_GPIO_F), 0);

	zassert_equal(gpio_pin_configure(gpio, AD74416H_GPIO_B, GPIO_INPUT | GPIO_PULL_UP),
		      -ENOTSUP);
	zassert_equal(gpio_pin_interrupt_configure(gpio, AD74416H_GPIO_B, GPIO_INT_EDGE_BOTH),
		      -ENOTSUP);
}

ZTEST(ad74416h, test_gpio_din_do)
{
	zassert_ok(gpio_pin_configure(gpio, AD74416H_GPIO_DIN(2), GPIO_INPUT));
	zassert_equal(gpio_pin_configure(gpio, AD74416H_GPIO_DIN(2), GPIO_OUTPUT), -ENOTSUP);

	ad74416h_emul_din_set(emul, BIT(2));
	zassert_equal(gpio_pin_get(gpio, AD74416H_GPIO_DIN(2)), 1);
	zassert_equal(gpio_pin_get(gpio, AD74416H_GPIO_DIN(1)), 0);

	zassert_equal(gpio_pin_configure(gpio, AD74416H_GPIO_DO(1), GPIO_INPUT), -ENOTSUP);
	zassert_ok(gpio_pin_configure(gpio, AD74416H_GPIO_DO(1), GPIO_OUTPUT_INACTIVE));
	zassert_equal(reg(AD74416H_REG_DO_EXT_CONFIG(1)) &
			      (AD74416H_DO_MODE | AD74416H_DO_DATA | AD74416H_DO_SRC_SEL),
		      AD74416H_DO_MODE);
	zassert_ok(gpio_pin_set(gpio, AD74416H_GPIO_DO(1), 1));
	zassert_true(reg(AD74416H_REG_DO_EXT_CONFIG(1)) & AD74416H_DO_DATA);
	zassert_equal(gpio_pin_get(gpio, AD74416H_GPIO_DO(1)), 1);
	zassert_ok(gpio_pin_configure(gpio, AD74416H_GPIO_DO(1), GPIO_DISCONNECTED));
	zassert_false(reg(AD74416H_REG_DO_EXT_CONFIG(1)) & (AD74416H_DO_MODE | AD74416H_DO_DATA));
}

ZTEST(ad74416h, test_din)
{
	uint32_t count;
	uint8_t state;

	ad74416h_emul_din_set(emul, 0x5);
	zassert_ok(ad74416h_din_get(dev, &state));
	zassert_equal(state, 0x5);

	ad74416h_emul_din_counter_set(emul, 2, 0x12345678);
	zassert_ok(ad74416h_din_counter_get(dev, 2, &count));
	zassert_equal(count, 0x12345678);

	zassert_ok(ad74416h_din_threshold_set(dev, 2, 60, false));
	zassert_equal(reg(AD74416H_REG_DIN_CONFIG1(2)) &
			      (AD74416H_DIN_THRESH_MODE | AD74416H_DIN_COMP_THRESH),
		      60);
	zassert_equal(ad74416h_din_threshold_set(dev, 2, 99, false), -EINVAL);
}

ZTEST(ad74416h, test_do_recover)
{
	/* A DO_TIMEOUT switches the output off and clears DO_MODE */
	ad74416h_emul_reg_set(emul, AD74416H_REG_DO_EXT_CONFIG(3),
			      reg(AD74416H_REG_DO_EXT_CONFIG(3)) & ~AD74416H_DO_MODE);

	zassert_ok(ad74416h_do_recover(dev, 3, true));
	zassert_equal(reg(AD74416H_REG_DO_EXT_CONFIG(3)) & (AD74416H_DO_MODE | AD74416H_DO_DATA),
		      AD74416H_DO_MODE | AD74416H_DO_DATA);
}

static K_SEM_DEFINE(alert_sem, 0, 1);
static struct ad74416h_alerts alert_seen;

static void alert_cb(const struct device *d, const struct ad74416h_alerts *alerts, void *ud)
{
	ARG_UNUSED(d);
	ARG_UNUSED(ud);

	alert_seen = *alerts;
	k_sem_give(&alert_sem);
}

ZTEST(ad74416h, test_alerts)
{
	struct ad74416h_alerts alerts;

	k_sem_reset(&alert_sem);
	zassert_ok(ad74416h_alert_callback_set(dev, alert_cb, NULL));

	ad74416h_emul_channel_alert_set(emul, 1, AD74416H_CH_ALERT_ANALOG_IO_OC);
	zassert_ok(k_sem_take(&alert_sem, K_MSEC(100)), "no alert callback");

	zassert_true(alert_seen.alert & AD74416H_ALERT_CHANNEL(1));
	zassert_equal(alert_seen.channel[1], AD74416H_CH_ALERT_ANALOG_IO_OC);

	zassert_ok(ad74416h_alerts_clear(dev, &alert_seen));
	zassert_ok(ad74416h_alerts_get(dev, &alerts));
	zassert_equal(alerts.alert, 0);
	zassert_equal(alerts.channel[1], 0);

	/* A masked alert stays in its register but off the summary bit */
	memset(&alerts, 0, sizeof(alerts));
	alerts.supply = AD74416H_SUPPLY_DO_VDD_ERR;
	zassert_ok(ad74416h_alerts_mask_set(dev, &alerts));
	ad74416h_emul_supply_alert_set(emul, AD74416H_SUPPLY_DO_VDD_ERR);
	zassert_ok(ad74416h_alerts_get(dev, &alerts));
	zassert_equal(alerts.supply, AD74416H_SUPPLY_DO_VDD_ERR);
	zassert_false(alerts.alert & AD74416H_ALERT_SUPPLY_ERR);
	zassert_ok(ad74416h_alerts_clear(dev, &alerts));
}

ZTEST(ad74416h, test_status_bits)
{
	uint16_t val;

	ad74416h_emul_channel_alert_set(emul, 0, AD74416H_CH_ALERT_ANALOG_IO_SC);
	zassert_ok(ad74416h_reg_read(dev, AD74416H_REG_SCRATCH(0), &val));
	/* ALERT and CHANNEL_ALERT */
	zassert_equal(ad74416h_last_status(dev), BIT(3) | BIT(1));
}

ZTEST(ad74416h, test_device_control)
{
	zassert_ok(ad74416h_wdt_enable(dev, 0x5));
	zassert_equal(reg(AD74416H_REG_WDT_CONFIG), AD74416H_WDT_EN | 0x5);
	zassert_ok(ad74416h_wdt_disable(dev));
	zassert_equal(reg(AD74416H_REG_WDT_CONFIG), 0x5);
	zassert_equal(ad74416h_wdt_enable(dev, 0xB), -EINVAL);

	zassert_ok(ad74416h_thermal_reset_enable(dev, true));
	zassert_equal(reg(AD74416H_REG_THERM_RST), AD74416H_THERM_RST_EN);

	/* adi,internal-ref keeps REF_EN set */
	zassert_ok(ad74416h_power_optim_set(dev, AD74416H_PWR_SENSE_HF_OPT(2)));
	zassert_equal(reg(AD74416H_REG_PWR_OPTIM_CONFIG),
		      AD74416H_PWR_SENSE_HF_OPT(2) | AD74416H_PWR_REF_EN);
	zassert_ok(ad74416h_power_optim_set(dev, 0));
	zassert_equal(reg(AD74416H_REG_PWR_OPTIM_CONFIG), AD74416H_PWR_REF_EN);
}

/* Unit conversions, against values from the data sheet tables */

ZTEST(ad74416h, test_convert_adc)
{
	/* Table 24 */
	zassert_equal(ad74416h_adc_code_to_nv(AD74416H_ADC_RANGE_0_12V, 0x800000), 6000000000LL);
	zassert_equal(ad74416h_adc_code_to_nv(AD74416H_ADC_RANGE_PM_12V, 0), -12000000000LL);
	zassert_equal(ad74416h_adc_code_to_nv(AD74416H_ADC_RANGE_PM_12V, 0x800000), 0);
	zassert_equal(ad74416h_adc_code_to_nv(AD74416H_ADC_RANGE_NEG_312MV_0, 0x800000),
		      -156250000);
	zassert_equal(ad74416h_adc_code_to_nv(AD74416H_ADC_RANGE_0_625MV, 0x800000), 312500000);
	zassert_within(ad74416h_adc_code_to_nv(AD74416H_ADC_RANGE_PM_104MV, 0), -104166667, 1);

	/* Table 25: 0x0 is 0 mA, full scale is 26.04 mA in the 0.3125 V range */
	zassert_equal(ad74416h_adc_code_to_na(AD74416H_ADC_RANGE_0_312MV, 0, 12000), 0);
	zassert_within(ad74416h_adc_code_to_na(AD74416H_ADC_RANGE_0_312MV, 0x800000, 12000),
		       13020833, 1);
}

ZTEST(ad74416h, test_convert_rtd)
{
	/* Pt100 range 0..0.625 V (gain 4): full scale is 2012 / 4 = 503 ohm */
	zassert_within(ad74416h_rtd_code_to_mohm(AD74416H_ADC_RANGE_0_625MV, 0x800000), 251500, 1);
	/* Pt1000 range 0..12 V (gain 1/4.8): half scale is 2012 * 4.8 / 2 */
	zassert_within(ad74416h_rtd_code_to_mohm(AD74416H_ADC_RANGE_0_12V, 0x800000), 4828800, 1);
	/* Cu10 range +-104 mV (gain 24): midscale is 0 ohm */
	zassert_equal(ad74416h_rtd_code_to_mohm(AD74416H_ADC_RANGE_PM_104MV, 0x800000), 0);
	zassert_within(ad74416h_rtd_code_to_mohm(AD74416H_ADC_RANGE_PM_104MV, 0xC00000), 41917, 1);
}

ZTEST(ad74416h, test_convert_diag)
{
	/* Table 30 */
	/* Code 2616 is 25.03 degC */
	zassert_within(ad74416h_diag_code_to_value(AD74416H_DIAG_TEMP, 2616, 0), 25028, 1);
	zassert_within(ad74416h_diag_code_to_value(AD74416H_DIAG_AVDD_HI, 32768, 0), 24038462, 1);
	zassert_within(ad74416h_diag_code_to_value(AD74416H_DIAG_DO_VDD, 32768, 0), 19531250, 1);
	zassert_within(ad74416h_diag_code_to_value(AD74416H_DIAG_DVCC, 32768, 0), 4166667, 1);
	zassert_equal(ad74416h_diag_code_to_value(AD74416H_DIAG_AVSS, 0, 0), -20000000);
	zassert_equal(ad74416h_diag_code_to_value(AD74416H_DIAG_VSENSEN, 32768, 0), 5000000);
	zassert_equal(ad74416h_diag_code_to_value(AD74416H_DIAG_VSENSEP, 32768, INT32_MIN),
		      5000000);
	zassert_equal(ad74416h_diag_code_to_value(AD74416H_DIAG_VSENSEP, 32768, 24000000), 6000000);
	/* 0.25 V across 0.15 ohm = 1.6667 A */
	zassert_within(ad74416h_diag_code_to_value(AD74416H_DIAG_I_DO_SRC, 32768, 150), 1666667, 1);
}

ZTEST(ad74416h, test_convert_dac_din)
{
	uint16_t code;
	uint8_t th;

	/* Table 31 */
	zassert_ok(ad74416h_dac_uv_to_code(true, 0, &code));
	zassert_equal(code, 0x8000);
	zassert_ok(ad74416h_dac_uv_to_code(false, 6000000, &code));
	zassert_equal(code, 0x8000);
	zassert_ok(ad74416h_dac_uv_to_code(false, 12000000, &code));
	zassert_equal(code, 0xFFFF);
	zassert_equal(ad74416h_dac_uv_to_code(false, -1, &code), -ERANGE);
	zassert_ok(ad74416h_dac_ua_to_code(12500, &code));
	zassert_equal(code, 0x8000);
	zassert_equal(ad74416h_dac_ua_to_code(25001, &code), -ERANGE);

	/* 8.5 V fixed threshold is the recommended 0x37 for IEC 61131-2 */
	zassert_ok(ad74416h_din_threshold_fixed_mv_to_code(8500, &th));
	zassert_equal(th, 0x37);
	zassert_ok(ad74416h_din_threshold_avdd_mv_to_code(12000, 24000, &th));
	zassert_equal(th, 73);
	zassert_equal(ad74416h_din_threshold_fixed_mv_to_code(31000, &th), -ERANGE);
}
