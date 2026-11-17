/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Analog Devices AD74416H core driver: SPI framing, reset, channel
 * configuration, ADC sequencing, and alerts. The ADC, DAC and GPIO child
 * drivers are built on the API in <zephyr/drivers/mfd/ad74416h.h>.
 *
 * SPI frames are 40 bits: [D39:D38] 0, [D37:D36] device address,
 * [D35:D32] reserved (write) or status (read), [D31:D24] register address,
 * [D23:D8] data, [D7:D0] CRC-8 (x^8 + x^2 + x + 1, seed 0). Reads are two
 * stage: write READ_SELECT, then the next frame returns the register.
 */

#define DT_DRV_COMPAT adi_ad74416h

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/mfd/ad74416h.h>

LOG_MODULE_REGISTER(mfd_ad74416h, CONFIG_MFD_LOG_LEVEL);

#define FRAME_LEN 5
#define CRC_POLY  0x07

#define FRAME_RD_MARKER GENMASK(7, 6) /* byte 0 of a read frame: 0b10 */
#define FRAME_RD_ONE    BIT(7)
#define FRAME_DEV_ADDR  GENMASK(5, 4)
#define FRAME_STATUS    GENMASK(3, 0)

#define HART_SETTLE_TIMEOUT_MS 100
#define ADC_POLL_INTERVAL_US   500

struct ad74416h_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec alert_gpio;
	struct gpio_dt_spec adc_rdy_gpio;
	uint8_t dev_addr;
	bool internal_ref;
	bool thermal_reset;
	uint8_t diag_assign[AD74416H_NUM_DIAGS];
	uint8_t diag_rate;
	/* Channels configured in devicetree, bit n is channel n */
	uint8_t ch_mask;
	struct ad74416h_channel_config ch[AD74416H_NUM_CHANNELS];
};

struct ad74416h_data {
	const struct device *dev;
	struct k_mutex lock;
	struct ad74416h_channel_config ch[AD74416H_NUM_CHANNELS];
	uint8_t diag_rate;
	uint8_t last_status;
	bool continuous;
	struct k_sem adc_rdy_sem;
	struct gpio_callback adc_rdy_cb;
	struct gpio_callback alert_gpio_cb;
	struct k_work alert_work;
	ad74416h_alert_cb_t alert_cb;
	void *alert_user_data;
};

/* SPI framing */

static void frame_build(const struct device *dev, uint8_t reg, uint16_t val,
			uint8_t frame[FRAME_LEN])
{
	const struct ad74416h_config *config = dev->config;

	frame[0] = FIELD_PREP(FRAME_DEV_ADDR, config->dev_addr);
	frame[1] = reg;
	sys_put_be16(val, &frame[2]);
	frame[4] = crc8(frame, 4, CRC_POLY, 0, false);
}

static int frame_xfer(const struct device *dev, const uint8_t tx[FRAME_LEN], uint8_t rx[FRAME_LEN])
{
	const struct ad74416h_config *config = dev->config;
	const struct spi_buf tx_buf = {.buf = (void *)tx, .len = FRAME_LEN};
	const struct spi_buf rx_buf = {.buf = rx, .len = FRAME_LEN};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

	/* One transaction per frame: SYNC must rise after every 40 clocks */
	return spi_transceive_dt(&config->spi, &tx_set, rx != NULL ? &rx_set : NULL);
}

static int frame_parse(const struct device *dev, const uint8_t rx[FRAME_LEN], uint8_t reg,
		       uint16_t *val)
{
	const struct ad74416h_config *config = dev->config;
	struct ad74416h_data *data = dev->data;

	if (crc8(rx, 4, CRC_POLY, 0, false) != rx[4]) {
		LOG_ERR("CRC error reading 0x%02x", reg);
		return -EIO;
	}

	if ((rx[0] & FRAME_RD_MARKER) != FRAME_RD_ONE ||
	    FIELD_GET(FRAME_DEV_ADDR, rx[0]) != config->dev_addr || rx[1] != reg) {
		LOG_ERR("Bad read frame for 0x%02x: %02x %02x", reg, rx[0], rx[1]);
		return -EIO;
	}

	data->last_status = FIELD_GET(FRAME_STATUS, rx[0]);
	*val = sys_get_be16(&rx[2]);
	return 0;
}

int ad74416h_reg_write(const struct device *dev, uint8_t reg, uint16_t val)
{
	struct ad74416h_data *data = dev->data;
	uint8_t tx[FRAME_LEN];
	int ret;

	frame_build(dev, reg, val, tx);

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = frame_xfer(dev, tx, NULL);
	k_mutex_unlock(&data->lock);

	return ret;
}

int ad74416h_reg_read_multi(const struct device *dev, const uint8_t *regs, uint16_t *vals,
			    size_t count)
{
	struct ad74416h_data *data = dev->data;
	uint8_t tx[FRAME_LEN];
	uint8_t rx[FRAME_LEN];
	int ret = 0;

	if (count == 0) {
		return 0;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Request stage for the first register */
	frame_build(dev, AD74416H_REG_READ_SELECT, regs[0], tx);
	ret = frame_xfer(dev, tx, NULL);

	/*
	 * Each read stage returns register i and, through its write to
	 * READ_SELECT, requests register i + 1. The last one is a NOP.
	 */
	for (size_t i = 0; ret == 0 && i < count; i++) {
		if (i + 1 < count) {
			frame_build(dev, AD74416H_REG_READ_SELECT, regs[i + 1], tx);
		} else {
			frame_build(dev, AD74416H_REG_NOP, 0, tx);
		}

		ret = frame_xfer(dev, tx, rx);
		if (ret == 0) {
			ret = frame_parse(dev, rx, regs[i], &vals[i]);
		}
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_reg_read(const struct device *dev, uint8_t reg, uint16_t *val)
{
	return ad74416h_reg_read_multi(dev, &reg, val, 1);
}

int ad74416h_reg_update(const struct device *dev, uint8_t reg, uint16_t mask, uint16_t val)
{
	struct ad74416h_data *data = dev->data;
	uint16_t cur;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ad74416h_reg_read(dev, reg, &cur);
	if (ret == 0) {
		ret = ad74416h_reg_write(dev, reg, (cur & ~mask) | (val & mask));
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

void ad74416h_lock(const struct device *dev)
{
	struct ad74416h_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
}

void ad74416h_unlock(const struct device *dev)
{
	struct ad74416h_data *data = dev->data;

	k_mutex_unlock(&data->lock);
}

uint8_t ad74416h_last_status(const struct device *dev)
{
	struct ad74416h_data *data = dev->data;

	return data->last_status;
}

/* Channel configuration */

static bool ch_func_valid(uint8_t func)
{
	return func <= AD74416H_CH_FUNC_IIN_LOOP_PWR_HART && func != 0x6;
}

void ad74416h_channel_config_init(struct ad74416h_channel_config *cfg)
{
	*cfg = (struct ad74416h_channel_config){
		.func = AD74416H_CH_FUNC_HIGH_IMP,
		.avdd_select = -1,
		.dac_code = -1,
		.adc_range = -1,
		.adc_mux = -1,
		.adc_rate = AD74416H_ADC_RATE_20SPS,
		.din_comparator = -1,
		.din_invert = -1,
		.din_threshold = -1,
		.din_debounce_time = 0xB,
		.do_t2 = 0x17,
	};
}

static int channel_config_check(const struct ad74416h_channel_config *cfg)
{
	if (!ch_func_valid(cfg->func) || cfg->avdd_select > AD74416H_AVDD_TRACK ||
	    cfg->slew > AD74416H_SLEW_HART || cfg->slew_step > 3 || cfg->slew_rate > 3 ||
	    cfg->dac_code > UINT16_MAX || cfg->adc_range > AD74416H_ADC_RANGE_PM_2V5 ||
	    cfg->adc_mux > AD74416H_ADC_MUX_AGND_TO_AGND || cfg->adc_rate > 0xF ||
	    cfg->din_threshold > AD74416H_DIN_COMP_THRESH_MAX || cfg->din_sink > 31 ||
	    cfg->din_debounce_time > 31 || cfg->do_t1 > 31 || cfg->do_t2 > 31 ||
	    cfg->burnout_viout > AD74416H_BURNOUT_10UA ||
	    cfg->burnout_vsensen > AD74416H_BURNOUT_10UA) {
		return -EINVAL;
	}

	return 0;
}

static void wait_us(uint32_t us)
{
	k_sleep(K_USEC(us));
}

static int hart_slew_settled_wait(const struct device *dev, uint8_t ch)
{
	uint16_t out;
	int ret;

	for (int i = 0; i < HART_SETTLE_TIMEOUT_MS; i++) {
		ret = ad74416h_reg_read(dev, AD74416H_REG_OUTPUT_CONFIG(ch), &out);
		if (ret != 0) {
			return ret;
		}
		if ((out & AD74416H_OUT_HART_COMPL_SETTLED) != 0) {
			return 0;
		}
		k_msleep(1);
	}

	LOG_ERR("Channel %u: HART compliant slew did not settle", ch);
	return -ETIMEDOUT;
}

static int slew_write(const struct device *dev, uint8_t ch, uint8_t slew, uint8_t step,
		      uint8_t rate)
{
	int ret;

	if (slew == AD74416H_SLEW_HART) {
		ret = hart_slew_settled_wait(dev, ch);
		if (ret != 0) {
			return ret;
		}
	}

	return ad74416h_reg_update(dev, AD74416H_REG_OUTPUT_CONFIG(ch),
				   AD74416H_OUT_SLEW_EN | AD74416H_OUT_SLEW_LIN_STEP |
					   AD74416H_OUT_SLEW_LIN_RATE,
				   FIELD_PREP(AD74416H_OUT_SLEW_EN, slew) |
					   FIELD_PREP(AD74416H_OUT_SLEW_LIN_STEP, step) |
					   FIELD_PREP(AD74416H_OUT_SLEW_LIN_RATE, rate));
}

static int channel_post_func(const struct device *dev, uint8_t ch,
			     const struct ad74416h_channel_config *cfg)
{
	uint16_t val;
	int ret;

	/* ADC: the function loaded its default range and input */
	ret = ad74416h_reg_read(dev, AD74416H_REG_ADC_CONFIG(ch), &val);
	if (ret != 0) {
		return ret;
	}
	if (cfg->adc_range >= 0) {
		val = (val & ~AD74416H_ADC_CONV_RANGE) |
		      FIELD_PREP(AD74416H_ADC_CONV_RANGE, cfg->adc_range);
	}
	if (cfg->adc_mux >= 0) {
		val = (val & ~AD74416H_ADC_CONV_MUX) |
		      FIELD_PREP(AD74416H_ADC_CONV_MUX, cfg->adc_mux);
	}
	val = (val & ~AD74416H_ADC_CONV_RATE) | FIELD_PREP(AD74416H_ADC_CONV_RATE, cfg->adc_rate);
	ret = ad74416h_reg_write(dev, AD74416H_REG_ADC_CONFIG(ch), val);
	if (ret != 0) {
		return ret;
	}

	/* Resistance measurement. 2-wire must be set within 200 us. */
	ret = ad74416h_reg_write(dev, AD74416H_REG_RTD_CONFIG(ch),
				 FIELD_PREP(AD74416H_RTD_ADC_REF, cfg->rtd_ref_1v) |
					 FIELD_PREP(AD74416H_RTD_MODE_SEL, cfg->rtd_2wire) |
					 FIELD_PREP(AD74416H_RTD_EXC_SWAP, cfg->rtd_exc_swap) |
					 FIELD_PREP(AD74416H_RTD_CURRENT, !cfg->rtd_500ua));
	if (ret != 0) {
		return ret;
	}

	/* Digital input */
	ret = ad74416h_reg_read(dev, AD74416H_REG_DIN_CONFIG0(ch), &val);
	if (ret != 0) {
		return ret;
	}
	val &= AD74416H_DIN_INV_COMP_OUT | AD74416H_DIN_COMPARATOR_EN;
	if (cfg->din_invert >= 0) {
		WRITE_BIT(val, 14, cfg->din_invert);
	}
	if (cfg->din_comparator >= 0) {
		WRITE_BIT(val, 13, cfg->din_comparator);
	}
	val |= FIELD_PREP(AD74416H_DIN_COUNT_EN, cfg->din_counter) |
	       FIELD_PREP(AD74416H_DIN_SINK_RANGE, cfg->din_sink_range_high) |
	       FIELD_PREP(AD74416H_DIN_SINK, cfg->din_sink) |
	       FIELD_PREP(AD74416H_DIN_DEBOUNCE_MODE, cfg->din_debounce_mode1) |
	       FIELD_PREP(AD74416H_DIN_DEBOUNCE_TIME, cfg->din_debounce_time);
	ret = ad74416h_reg_write(dev, AD74416H_REG_DIN_CONFIG0(ch), val);
	if (ret != 0) {
		return ret;
	}

	ret = ad74416h_reg_read(dev, AD74416H_REG_DIN_CONFIG1(ch), &val);
	if (ret != 0) {
		return ret;
	}
	val &= AD74416H_DIN_COMP_THRESH;
	if (cfg->din_threshold >= 0) {
		val = FIELD_PREP(AD74416H_DIN_COMP_THRESH, cfg->din_threshold);
	}
	val |= FIELD_PREP(AD74416H_DIN_INPUT_SELECT, cfg->din_input_viout) |
	       FIELD_PREP(AD74416H_DIN_SC_DET_EN, cfg->din_sc_detect) |
	       FIELD_PREP(AD74416H_DIN_OC_DET_EN, cfg->din_oc_detect) |
	       FIELD_PREP(AD74416H_DIN_THRESH_MODE, cfg->din_threshold_fixed);
	ret = ad74416h_reg_write(dev, AD74416H_REG_DIN_CONFIG1(ch), val);
	if (ret != 0) {
		return ret;
	}

	/* Burnout currents and FET leakage compensation */
	ret = ad74416h_reg_write(
		dev, AD74416H_REG_I_BURNOUT_CONFIG(ch),
		FIELD_PREP(AD74416H_BRN_SEN_VSENSEN_CURR, cfg->burnout_vsensen) |
			FIELD_PREP(AD74416H_BRN_SEN_VSENSEN_POL, cfg->burnout_vsensen_source) |
			FIELD_PREP(AD74416H_BRN_VIOUT_CURR, cfg->burnout_viout) |
			FIELD_PREP(AD74416H_BRN_VIOUT_POL, cfg->burnout_viout_source));
	if (ret != 0) {
		return ret;
	}

	ret = ad74416h_reg_write(dev, AD74416H_REG_FET_LKG_COMP(ch),
				 FIELD_PREP(AD74416H_FET_SRC_LKG_COMP_EN, cfg->fet_leakage_comp));
	if (ret != 0) {
		return ret;
	}

	/* External FET digital output, FET off */
	return ad74416h_reg_write(dev, AD74416H_REG_DO_EXT_CONFIG(ch),
				  FIELD_PREP(AD74416H_DO_T2, cfg->do_t2) |
					  FIELD_PREP(AD74416H_DO_T1, cfg->do_t1) |
					  FIELD_PREP(AD74416H_DO_SRC_SEL, cfg->do_gpio_control) |
					  FIELD_PREP(AD74416H_DO_MODE, cfg->do_enable));
}

int ad74416h_channel_configure(const struct device *dev, uint8_t ch,
			       const struct ad74416h_channel_config *cfg)
{
	struct ad74416h_data *data = dev->data;
	uint16_t dac_code;
	uint16_t val;
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	ret = channel_config_check(cfg);
	if (ret != 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Leaving a digital output: disable it first */
	ret = ad74416h_reg_read(dev, AD74416H_REG_DO_EXT_CONFIG(ch), &val);
	if (ret != 0) {
		goto out;
	}
	if ((val & AD74416H_DO_MODE) != 0) {
		ret = ad74416h_reg_write(dev, AD74416H_REG_DO_EXT_CONFIG(ch),
					 val & ~(AD74416H_DO_MODE | AD74416H_DO_DATA));
		if (ret != 0) {
			goto out;
		}
	}

	/* High impedance is required between functions */
	ret = ad74416h_reg_write(dev, AD74416H_REG_CH_FUNC_SETUP(ch), AD74416H_CH_FUNC_HIGH_IMP);
	if (ret != 0) {
		goto out;
	}

	/* VOUT_RANGE and VIOUT_DRV_EN_DLY may only change in high impedance */
	ret = ad74416h_reg_write(
		dev, AD74416H_REG_OUTPUT_CONFIG(ch),
		FIELD_PREP(AD74416H_OUT_ALARM_DEG_PERIOD, cfg->alarm_deglitch_20ms) |
			FIELD_PREP(AD74416H_OUT_VOUT_4W_EN, cfg->vout_4wire) |
			FIELD_PREP(AD74416H_OUT_VIOUT_DRV_EN_DLY, cfg->viout_drv_en_dly_200us) |
			FIELD_PREP(AD74416H_OUT_VOUT_RANGE, cfg->vout_bipolar) |
			FIELD_PREP(AD74416H_OUT_I_LIMIT, cfg->i_limit_8ma));
	if (ret != 0) {
		goto out;
	}

	if (cfg->dac_code >= 0) {
		dac_code = cfg->dac_code;
	} else if (cfg->func == AD74416H_CH_FUNC_VOUT && cfg->vout_bipolar) {
		dac_code = 0x8000; /* 0 V */
	} else {
		dac_code = 0;
	}

	ret = ad74416h_reg_write(dev, AD74416H_REG_DAC_CODE(ch), dac_code);
	if (ret != 0) {
		goto out;
	}

	wait_us(AD74416H_CH_INIT_TIME_US);

	if (cfg->func != AD74416H_CH_FUNC_HIGH_IMP) {
		ret = ad74416h_reg_write(dev, AD74416H_REG_CH_FUNC_SETUP(ch), cfg->func);
		if (ret != 0) {
			goto out;
		}
	}

	ret = channel_post_func(dev, ch, cfg);
	if (ret != 0) {
		goto out;
	}

	wait_us(cfg->func == AD74416H_CH_FUNC_IOUT_HART ? AD74416H_CH_INIT_TIME_HART_US
							: AD74416H_CH_INIT_TIME_US);

	if (cfg->avdd_select >= 0) {
		ret = ad74416h_reg_update(dev, AD74416H_REG_OUTPUT_CONFIG(ch),
					  AD74416H_OUT_AVDD_SELECT,
					  FIELD_PREP(AD74416H_OUT_AVDD_SELECT, cfg->avdd_select));
		if (ret != 0) {
			goto out;
		}
	}

	ret = slew_write(dev, ch, cfg->slew, cfg->slew_step, cfg->slew_rate);
	if (ret != 0) {
		goto out;
	}

	data->ch[ch] = *cfg;
	data->ch[ch].dac_code = dac_code;

out:
	if (ret != 0) {
		LOG_ERR("Channel %u: configuration failed (%d)", ch, ret);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_channel_config_get(const struct device *dev, uint8_t ch,
				struct ad74416h_channel_config *cfg)
{
	struct ad74416h_data *data = dev->data;

	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	*cfg = data->ch[ch];
	k_mutex_unlock(&data->lock);
	return 0;
}

int ad74416h_channel_func_get(const struct device *dev, uint8_t ch, uint8_t *func)
{
	uint16_t val;
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	ret = ad74416h_reg_read(dev, AD74416H_REG_CH_FUNC_SETUP(ch), &val);
	if (ret == 0) {
		*func = FIELD_GET(AD74416H_CH_FUNC, val);
	}
	return ret;
}

/* ADC */

int ad74416h_adc_channel_set(const struct device *dev, uint8_t ch, uint8_t range, uint8_t rate,
			     uint8_t mux)
{
	struct ad74416h_data *data = dev->data;
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS || range > AD74416H_ADC_RANGE_PM_2V5 || rate > 0xF ||
	    mux > AD74416H_ADC_MUX_AGND_TO_AGND) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->continuous) {
		/* The data sheet requires stopping continuous conversions first */
		ret = -EBUSY;
	} else {
		ret = ad74416h_reg_update(dev, AD74416H_REG_ADC_CONFIG(ch),
					  AD74416H_ADC_CONV_RATE | AD74416H_ADC_CONV_RANGE |
						  AD74416H_ADC_CONV_MUX,
					  FIELD_PREP(AD74416H_ADC_CONV_RATE, rate) |
						  FIELD_PREP(AD74416H_ADC_CONV_RANGE, range) |
						  FIELD_PREP(AD74416H_ADC_CONV_MUX, mux));
	}

	if (ret == 0) {
		data->ch[ch].adc_range = range;
		data->ch[ch].adc_rate = rate;
		data->ch[ch].adc_mux = mux;
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_adc_range_get(const struct device *dev, uint8_t ch, uint8_t *range)
{
	uint16_t val;
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	ret = ad74416h_reg_read(dev, AD74416H_REG_ADC_CONFIG(ch), &val);
	if (ret == 0) {
		*range = FIELD_GET(AD74416H_ADC_CONV_RANGE, val);
	}
	return ret;
}

int ad74416h_diag_assign(const struct device *dev, uint8_t slot, uint8_t diag)
{
	if (slot >= AD74416H_NUM_DIAGS || diag > AD74416H_DIAG_AVDD_CH) {
		return -EINVAL;
	}

	return ad74416h_reg_update(dev, AD74416H_REG_DIAG_ASSIGN, AD74416H_DIAG_ASSIGN(slot),
				   (uint16_t)diag << (4 * slot));
}

int ad74416h_diag_get(const struct device *dev, uint8_t slot, uint8_t *diag)
{
	uint16_t val;
	int ret;

	if (slot >= AD74416H_NUM_DIAGS) {
		return -EINVAL;
	}

	ret = ad74416h_reg_read(dev, AD74416H_REG_DIAG_ASSIGN, &val);
	if (ret == 0) {
		*diag = (val >> (4 * slot)) & 0xF;
	}
	return ret;
}

int ad74416h_diag_rate_set(const struct device *dev, uint8_t rate)
{
	struct ad74416h_data *data = dev->data;

	if (rate > AD74416H_DIAG_RATE_19K2SPS) {
		return -EINVAL;
	}

	/* Written with the next conversion command */
	k_mutex_lock(&data->lock, K_FOREVER);
	data->diag_rate = rate;
	k_mutex_unlock(&data->lock);
	return 0;
}

static uint16_t conv_ctrl(struct ad74416h_data *data, uint8_t ch_mask, uint8_t diag_mask,
			  uint8_t seq)
{
	return FIELD_PREP(AD74416H_CONV_RATE_DIAG, data->diag_rate) |
	       FIELD_PREP(AD74416H_CONV_SEQ, seq) |
	       FIELD_PREP(AD74416H_CONV_DIAG_EN_ALL, diag_mask) |
	       FIELD_PREP(AD74416H_CONV_CH_EN_ALL, ch_mask);
}

static int live_status_wait(const struct device *dev, uint16_t mask, bool set, k_timepoint_t end)
{
	uint16_t live;
	int ret;

	while (true) {
		ret = ad74416h_reg_read(dev, AD74416H_REG_LIVE_STATUS, &live);
		if (ret != 0) {
			return ret;
		}
		if (((live & mask) != 0) == set) {
			return 0;
		}
		if (sys_timepoint_expired(end)) {
			return -ETIMEDOUT;
		}
		k_sleep(K_USEC(ADC_POLL_INTERVAL_US));
	}
}

int ad74416h_adc_results_read(const struct device *dev, uint8_t ch_mask, uint8_t diag_mask,
			      uint32_t results[8], uint8_t *seq_count)
{
	uint8_t regs[2 * AD74416H_NUM_CHANNELS + AD74416H_NUM_DIAGS];
	uint16_t vals[ARRAY_SIZE(regs)];
	size_t n = 0;
	int ret;

	if ((ch_mask & ~AD74416H_CONV_CH_EN_ALL) != 0 || (diag_mask & ~0xF) != 0) {
		return -EINVAL;
	}

	/* ADC_RESULT_UPRn must be read immediately before ADC_RESULTn */
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		if ((ch_mask & BIT(ch)) != 0) {
			regs[n++] = AD74416H_REG_ADC_RESULT_UPR(ch);
			regs[n++] = AD74416H_REG_ADC_RESULT(ch);
		}
	}
	for (int d = 0; d < AD74416H_NUM_DIAGS; d++) {
		if ((diag_mask & BIT(d)) != 0) {
			regs[n++] = AD74416H_REG_ADC_DIAG_RESULT(d);
		}
	}

	ret = ad74416h_reg_read_multi(dev, regs, vals, n);
	if (ret != 0) {
		return ret;
	}

	n = 0;
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		if ((ch_mask & BIT(ch)) != 0) {
			if (seq_count != NULL) {
				*seq_count = FIELD_GET(AD74416H_RES_CONV_SEQ_COUNT, vals[n]);
			}
			results[ch] =
				(FIELD_GET(AD74416H_RES_CONV_RES_UPR, vals[n]) << 16) | vals[n + 1];
			n += 2;
		}
	}
	for (int d = 0; d < AD74416H_NUM_DIAGS; d++) {
		if ((diag_mask & BIT(d)) != 0) {
			results[AD74416H_NUM_CHANNELS + d] = vals[n++];
		}
	}

	return 0;
}

int ad74416h_adc_convert(const struct device *dev, uint8_t ch_mask, uint8_t diag_mask,
			 uint32_t results[8], k_timeout_t timeout)
{
	const struct ad74416h_config *config = dev->config;
	struct ad74416h_data *data = dev->data;
	k_timepoint_t end = sys_timepoint_calc(timeout);
	int ret;

	if ((ch_mask & ~AD74416H_CONV_CH_EN_ALL) != 0 || (diag_mask & ~0xF) != 0 ||
	    (ch_mask | diag_mask) == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->continuous) {
		ret = -EBUSY;
		goto out;
	}

	/* A previous sequence or the ADC power-up must be finished */
	ret = live_status_wait(dev, AD74416H_LIVE_ADC_BUSY, false, end);
	if (ret != 0) {
		goto out;
	}

	k_sem_reset(&data->adc_rdy_sem);

	ret = ad74416h_reg_write(dev, AD74416H_REG_ADC_CONV_CTRL,
				 conv_ctrl(data, ch_mask, diag_mask, AD74416H_CONV_SEQ_SINGLE));
	if (ret != 0) {
		goto out;
	}

	if (config->adc_rdy_gpio.port != NULL) {
		ret = k_sem_take(&data->adc_rdy_sem, sys_timepoint_timeout(end));
		if (ret == -EAGAIN) {
			ret = -ETIMEDOUT;
		}
	} else {
		ret = live_status_wait(dev, AD74416H_LIVE_ADC_DATA_RDY, true, end);
	}
	if (ret != 0) {
		LOG_ERR("ADC sequence timed out");
		goto out;
	}

	ret = ad74416h_adc_results_read(dev, ch_mask, diag_mask, results, NULL);

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_adc_start_continuous(const struct device *dev, uint8_t ch_mask, uint8_t diag_mask)
{
	struct ad74416h_data *data = dev->data;
	int ret;

	if ((ch_mask & ~AD74416H_CONV_CH_EN_ALL) != 0 || (diag_mask & ~0xF) != 0 ||
	    (ch_mask | diag_mask) == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->continuous) {
		ret = -EBUSY;
	} else {
		ret = live_status_wait(dev, AD74416H_LIVE_ADC_BUSY, false,
				       sys_timepoint_calc(K_MSEC(200)));
	}
	if (ret == 0) {
		ret = ad74416h_reg_write(
			dev, AD74416H_REG_ADC_CONV_CTRL,
			conv_ctrl(data, ch_mask, diag_mask, AD74416H_CONV_SEQ_CONTINUOUS));
	}
	if (ret == 0) {
		data->continuous = true;
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_adc_stop(const struct device *dev, bool power_down)
{
	struct ad74416h_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ad74416h_reg_write(
		dev, AD74416H_REG_ADC_CONV_CTRL,
		conv_ctrl(data, 0, 0,
			  power_down ? AD74416H_CONV_SEQ_POWER_DOWN : AD74416H_CONV_SEQ_IDLE));
	if (ret == 0) {
		data->continuous = false;
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

/* DAC */

int ad74416h_dac_set(const struct device *dev, uint8_t ch, uint16_t code)
{
	struct ad74416h_data *data = dev->data;
	uint8_t regs[3];
	uint16_t vals[3];
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	regs[0] = AD74416H_REG_OUTPUT_CONFIG(ch);
	regs[1] = AD74416H_REG_DAC_CODE(ch);
	regs[2] = AD74416H_REG_DAC_ACTIVE(ch);

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ad74416h_reg_read_multi(dev, regs, vals, ARRAY_SIZE(regs));
	if (ret != 0) {
		goto out;
	}

	/* DAC_CODE must not change while linear slewing is in progress */
	if (FIELD_GET(AD74416H_OUT_SLEW_EN, vals[0]) == AD74416H_SLEW_LINEAR &&
	    vals[1] != vals[2]) {
		ret = -EBUSY;
		goto out;
	}

	ret = ad74416h_reg_write(dev, AD74416H_REG_DAC_CODE(ch), code);
	if (ret == 0) {
		data->ch[ch].dac_code = code;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_dac_active_get(const struct device *dev, uint8_t ch, uint16_t *code)
{
	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	return ad74416h_reg_read(dev, AD74416H_REG_DAC_ACTIVE(ch), code);
}

int ad74416h_dac_slew_set(const struct device *dev, uint8_t ch, uint8_t slew, uint8_t step,
			  uint8_t rate)
{
	struct ad74416h_data *data = dev->data;
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS || slew > AD74416H_SLEW_HART || step > 3 || rate > 3) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = slew_write(dev, ch, slew, step, rate);
	if (ret == 0) {
		data->ch[ch].slew = slew;
		data->ch[ch].slew_step = step;
		data->ch[ch].slew_rate = rate;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_dac_update_all(const struct device *dev)
{
	return ad74416h_reg_write(dev, AD74416H_REG_CMD_KEY, AD74416H_CMD_DAC_UPDATE);
}

/* Digital input and output */

int ad74416h_din_get(const struct device *dev, uint8_t *state)
{
	uint16_t val;
	int ret;

	ret = ad74416h_reg_read(dev, AD74416H_REG_DIN_COMP_OUT, &val);
	if (ret == 0) {
		*state = val & 0xF;
	}
	return ret;
}

int ad74416h_din_counter_get(const struct device *dev, uint8_t ch, uint32_t *count)
{
	uint8_t regs[2];
	uint16_t vals[2];
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	regs[0] = AD74416H_REG_DIN_COUNTER_UPR(ch);
	regs[1] = AD74416H_REG_DIN_COUNTER(ch);

	ret = ad74416h_reg_read_multi(dev, regs, vals, 2);
	if (ret == 0) {
		*count = ((uint32_t)vals[0] << 16) | vals[1];
	}
	return ret;
}

int ad74416h_din_threshold_set(const struct device *dev, uint8_t ch, uint8_t code, bool fixed)
{
	if (ch >= AD74416H_NUM_CHANNELS || code > AD74416H_DIN_COMP_THRESH_MAX) {
		return -EINVAL;
	}

	return ad74416h_reg_update(dev, AD74416H_REG_DIN_CONFIG1(ch),
				   AD74416H_DIN_THRESH_MODE | AD74416H_DIN_COMP_THRESH,
				   FIELD_PREP(AD74416H_DIN_THRESH_MODE, fixed) |
					   FIELD_PREP(AD74416H_DIN_COMP_THRESH, code));
}

int ad74416h_do_set(const struct device *dev, uint8_t ch, bool on)
{
	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	return ad74416h_reg_update(dev, AD74416H_REG_DO_EXT_CONFIG(ch), AD74416H_DO_DATA,
				   on ? AD74416H_DO_DATA : 0);
}

int ad74416h_do_recover(const struct device *dev, uint8_t ch, bool on)
{
	struct ad74416h_data *data = dev->data;
	int ret;

	if (ch >= AD74416H_NUM_CHANNELS) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ad74416h_reg_update(dev, AD74416H_REG_DO_EXT_CONFIG(ch), AD74416H_DO_DATA, 0);
	if (ret == 0) {
		ret = ad74416h_reg_update(dev, AD74416H_REG_DO_EXT_CONFIG(ch), AD74416H_DO_MODE,
					  AD74416H_DO_MODE);
	}
	if (ret == 0 && on) {
		ret = ad74416h_reg_update(dev, AD74416H_REG_DO_EXT_CONFIG(ch), AD74416H_DO_DATA,
					  AD74416H_DO_DATA);
	}
	if (ret == 0) {
		data->ch[ch].do_enable = true;
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

/* Alerts */

int ad74416h_alerts_get(const struct device *dev, struct ad74416h_alerts *alerts)
{
	uint8_t regs[2 + 2 * AD74416H_NUM_CHANNELS];
	uint16_t vals[ARRAY_SIZE(regs)];
	int ret;

	regs[0] = AD74416H_REG_ALERT_STATUS;
	regs[1] = AD74416H_REG_SUPPLY_ALERT_STATUS;
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		regs[2 + ch] = AD74416H_REG_CHANNEL_ALERT_STATUS(ch);
		regs[2 + AD74416H_NUM_CHANNELS + ch] = AD74416H_REG_HART_ALERT_STATUS(ch);
	}

	ret = ad74416h_reg_read_multi(dev, regs, vals, ARRAY_SIZE(regs));
	if (ret != 0) {
		return ret;
	}

	alerts->alert = vals[0];
	alerts->supply = vals[1];
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		alerts->channel[ch] = vals[2 + ch];
		alerts->hart[ch] = vals[2 + AD74416H_NUM_CHANNELS + ch];
	}

	return 0;
}

int ad74416h_alerts_clear(const struct device *dev, const struct ad74416h_alerts *alerts)
{
	struct ad74416h_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Detail registers before ALERT_STATUS */
	for (int ch = 0; ret == 0 && ch < AD74416H_NUM_CHANNELS; ch++) {
		if ((alerts->channel[ch] & AD74416H_CH_ALERT_ALL) != 0) {
			ret = ad74416h_reg_write(dev, AD74416H_REG_CHANNEL_ALERT_STATUS(ch),
						 alerts->channel[ch] & AD74416H_CH_ALERT_ALL);
		}
		if (ret == 0 && (alerts->hart[ch] & AD74416H_HART_ALERT_W1C) != 0) {
			ret = ad74416h_reg_write(dev, AD74416H_REG_HART_ALERT_STATUS(ch),
						 alerts->hart[ch] & AD74416H_HART_ALERT_W1C);
		}
	}
	if (ret == 0 && (alerts->supply & AD74416H_SUPPLY_ALL) != 0) {
		ret = ad74416h_reg_write(dev, AD74416H_REG_SUPPLY_ALERT_STATUS,
					 alerts->supply & AD74416H_SUPPLY_ALL);
	}
	if (ret == 0 && alerts->alert != 0) {
		ret = ad74416h_reg_write(dev, AD74416H_REG_ALERT_STATUS, alerts->alert);
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_alerts_mask_set(const struct device *dev, const struct ad74416h_alerts *masks)
{
	struct ad74416h_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ad74416h_reg_write(dev, AD74416H_REG_ALERT_MASK, masks->alert);
	if (ret == 0) {
		ret = ad74416h_reg_write(dev, AD74416H_REG_SUPPLY_ALERT_MASK, masks->supply);
	}
	for (int ch = 0; ret == 0 && ch < AD74416H_NUM_CHANNELS; ch++) {
		ret = ad74416h_reg_write(dev, AD74416H_REG_CHANNEL_ALERT_MASK(ch),
					 masks->channel[ch]);
		if (ret == 0) {
			ret = ad74416h_reg_write(dev, AD74416H_REG_HART_ALERT_MASK(ch),
						 masks->hart[ch]);
		}
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

int ad74416h_live_status_get(const struct device *dev, uint16_t *status)
{
	return ad74416h_reg_read(dev, AD74416H_REG_LIVE_STATUS, status);
}

int ad74416h_alert_callback_set(const struct device *dev, ad74416h_alert_cb_t cb, void *user_data)
{
	const struct ad74416h_config *config = dev->config;
	struct ad74416h_data *data = dev->data;

	if (config->alert_gpio.port == NULL) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->alert_cb = cb;
	data->alert_user_data = user_data;
	k_mutex_unlock(&data->lock);

	/* Report alerts that are already pending */
	if (cb != NULL && gpio_pin_get_dt(&config->alert_gpio) > 0) {
		k_work_submit(&data->alert_work);
	}

	return 0;
}

static void alert_work_handler(struct k_work *work)
{
	struct ad74416h_data *data = CONTAINER_OF(work, struct ad74416h_data, alert_work);
	const struct device *dev = data->dev;
	struct ad74416h_alerts alerts;
	ad74416h_alert_cb_t cb;
	void *user_data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	cb = data->alert_cb;
	user_data = data->alert_user_data;
	k_mutex_unlock(&data->lock);

	if (cb == NULL) {
		return;
	}

	ret = ad74416h_alerts_get(dev, &alerts);
	if (ret != 0) {
		LOG_ERR("Reading alerts failed (%d)", ret);
		return;
	}

	cb(dev, &alerts, user_data);
}

static void alert_gpio_handler(const struct device *port, struct gpio_callback *cb,
			       gpio_port_pins_t pins)
{
	struct ad74416h_data *data = CONTAINER_OF(cb, struct ad74416h_data, alert_gpio_cb);

	k_work_submit(&data->alert_work);
}

static void adc_rdy_gpio_handler(const struct device *port, struct gpio_callback *cb,
				 gpio_port_pins_t pins)
{
	struct ad74416h_data *data = CONTAINER_OF(cb, struct ad74416h_data, adc_rdy_cb);

	k_sem_give(&data->adc_rdy_sem);
}

/* Device control */

int ad74416h_get_id(const struct device *dev, uint8_t *silicon_rev, uint32_t *unique_id)
{
	static const uint8_t regs[] = {AD74416H_REG_SILICON_REV, AD74416H_REG_SILICON_ID0,
				       AD74416H_REG_SILICON_ID1};
	uint16_t vals[ARRAY_SIZE(regs)];
	int ret;

	ret = ad74416h_reg_read_multi(dev, regs, vals, ARRAY_SIZE(regs));
	if (ret != 0) {
		return ret;
	}

	if (silicon_rev != NULL) {
		*silicon_rev = FIELD_GET(AD74416H_SILICON_REV_ID, vals[0]);
	}
	if (unique_id != NULL) {
		*unique_id = FIELD_GET(AD74416H_SILICON_UID0, vals[1]) |
			     (FIELD_GET(AD74416H_SILICON_UID1, vals[2]) << 7) |
			     (FIELD_GET(AD74416H_SILICON_UID2, vals[2]) << 13);
	}
	return 0;
}

int ad74416h_wdt_enable(const struct device *dev, uint8_t timeout_code)
{
	if (timeout_code > 0xA) {
		return -EINVAL;
	}

	return ad74416h_reg_write(dev, AD74416H_REG_WDT_CONFIG,
				  AD74416H_WDT_EN | FIELD_PREP(AD74416H_WDT_TIMEOUT, timeout_code));
}

int ad74416h_wdt_disable(const struct device *dev)
{
	return ad74416h_reg_update(dev, AD74416H_REG_WDT_CONFIG, AD74416H_WDT_EN, 0);
}

int ad74416h_thermal_reset_enable(const struct device *dev, bool enable)
{
	return ad74416h_reg_write(dev, AD74416H_REG_THERM_RST, enable ? AD74416H_THERM_RST_EN : 0);
}

int ad74416h_power_optim_set(const struct device *dev, uint16_t pwr_optim_config)
{
	const struct ad74416h_config *config = dev->config;
	struct ad74416h_data *data = dev->data;
	uint16_t old;
	int ret;

	/* Without an external reference, clearing REF_EN would leave the device without one */
	if (config->internal_ref) {
		pwr_optim_config |= AD74416H_PWR_REF_EN;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ad74416h_reg_read(dev, AD74416H_REG_PWR_OPTIM_CONFIG, &old);
	if (ret == 0) {
		ret = ad74416h_reg_write(dev, AD74416H_REG_PWR_OPTIM_CONFIG,
					 pwr_optim_config & GENMASK(13, 0));
	}

	/* Buffers leaving standby need 100 us before the next conversion */
	if (ret == 0 && (old & ~pwr_optim_config & GENMASK(12, 0)) != 0) {
		k_sleep(K_USEC(100));
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static int hw_reset(const struct device *dev)
{
	const struct ad74416h_config *config = dev->config;
	struct ad74416h_data *data = dev->data;
	struct ad74416h_alerts alerts = {0};
	uint16_t val;
	int ret;

	if (config->reset_gpio.port != NULL) {
		ret = gpio_pin_set_dt(&config->reset_gpio, 1);
		if (ret != 0) {
			return ret;
		}
		k_busy_wait(2 * AD74416H_RESET_PULSE_US);
		ret = gpio_pin_set_dt(&config->reset_gpio, 0);
	} else {
		/* The two keys must be back to back */
		ret = ad74416h_reg_write(dev, AD74416H_REG_CMD_KEY, AD74416H_CMD_SW_RESET_KEY1);
		if (ret == 0) {
			ret = ad74416h_reg_write(dev, AD74416H_REG_CMD_KEY,
						 AD74416H_CMD_SW_RESET_KEY2);
		}
	}
	if (ret != 0) {
		return ret;
	}

	/* Any SPI access before the calibration memory is loaded sets CAL_MEM_ERR */
	k_sleep(K_USEC(AD74416H_RESET_TIME_US + AD74416H_RESET_TIME_US / 2));

	data->continuous = false;
	data->diag_rate = config->diag_rate;
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		ad74416h_channel_config_init(&data->ch[ch]);
	}

	ret = ad74416h_reg_read(dev, AD74416H_REG_GENERIC_ID, &val);
	if (ret != 0) {
		return ret;
	}
	if (FIELD_GET(AD74416H_GENERIC_ID, val) != AD74416H_GENERIC_ID_VALUE) {
		LOG_ERR("Unexpected GENERIC_ID %u",
			(unsigned int)FIELD_GET(AD74416H_GENERIC_ID, val));
		return -ENODEV;
	}

	/* A CAL_MEM_ERR that cannot be cleared is a calibration memory fault */
	ret = ad74416h_reg_write(dev, AD74416H_REG_SUPPLY_ALERT_STATUS, AD74416H_SUPPLY_ALL);
	if (ret == 0) {
		ret = ad74416h_reg_read(dev, AD74416H_REG_SUPPLY_ALERT_STATUS, &val);
	}
	if (ret != 0) {
		return ret;
	}
	if ((val & AD74416H_SUPPLY_CAL_MEM_ERR) != 0) {
		LOG_ERR("Calibration memory error, check the supplies");
		return -EIO;
	}

	ret = ad74416h_alerts_get(dev, &alerts);
	if (ret == 0) {
		alerts.alert = 0xFFFF;
		ret = ad74416h_alerts_clear(dev, &alerts);
	}
	return ret;
}

static int apply_dt_config(const struct device *dev)
{
	const struct ad74416h_config *config = dev->config;
	uint16_t diag = 0;
	int ret;

	if (config->internal_ref) {
		ret = ad74416h_reg_update(dev, AD74416H_REG_PWR_OPTIM_CONFIG, AD74416H_PWR_REF_EN,
					  AD74416H_PWR_REF_EN);
		if (ret != 0) {
			return ret;
		}
	}

	ret = ad74416h_thermal_reset_enable(dev, config->thermal_reset);
	if (ret != 0) {
		return ret;
	}

	for (int d = 0; d < AD74416H_NUM_DIAGS; d++) {
		diag |= (uint16_t)config->diag_assign[d] << (4 * d);
	}
	ret = ad74416h_reg_write(dev, AD74416H_REG_DIAG_ASSIGN, diag);
	if (ret != 0) {
		return ret;
	}

	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		if ((config->ch_mask & BIT(ch)) == 0) {
			continue;
		}
		ret = ad74416h_channel_configure(dev, ch, &config->ch[ch]);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

int ad74416h_reset(const struct device *dev)
{
	struct ad74416h_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = hw_reset(dev);
	if (ret == 0) {
		ret = apply_dt_config(dev);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int gpio_input_setup(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
			    gpio_callback_handler_t handler)
{
	int ret;

	if (spec->port == NULL) {
		return 0;
	}

	if (!gpio_is_ready_dt(spec)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(spec, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}

	gpio_init_callback(cb, handler, BIT(spec->pin));
	ret = gpio_add_callback_dt(spec, cb);
	if (ret != 0) {
		return ret;
	}

	return gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_TO_ACTIVE);
}

static int ad74416h_init(const struct device *dev)
{
	const struct ad74416h_config *config = dev->config;
	struct ad74416h_data *data = dev->data;
	uint8_t rev;
	int ret;

	data->dev = dev;
	k_mutex_init(&data->lock);
	k_sem_init(&data->adc_rdy_sem, 0, 1);
	k_work_init(&data->alert_work, alert_work_handler);

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	if (config->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->reset_gpio)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			return ret;
		}
	}

	ret = gpio_input_setup(&config->alert_gpio, &data->alert_gpio_cb, alert_gpio_handler);
	if (ret != 0) {
		LOG_ERR("ALERT pin setup failed (%d)", ret);
		return ret;
	}

	ret = gpio_input_setup(&config->adc_rdy_gpio, &data->adc_rdy_cb, adc_rdy_gpio_handler);
	if (ret != 0) {
		LOG_ERR("ADC_RDY pin setup failed (%d)", ret);
		return ret;
	}

	/* Device power-up time */
	k_sleep(K_USEC(AD74416H_RESET_TIME_US));

	ret = ad74416h_reset(dev);
	if (ret != 0) {
		LOG_ERR("Init failed (%d)", ret);
		return ret;
	}

	ret = ad74416h_get_id(dev, &rev, NULL);
	if (ret == 0) {
		LOG_INF("AD74416H silicon revision %u", rev);
	}
	return ret;
}

/* Channel nodes are the children with a channel function; the adc, dac and
 * gpio children have their own compatibles.
 */
#define AD74416H_IS_CH(node) DT_NODE_HAS_PROP(node, adi_ch_func)

#define AD74416H_CH_CFG(node)                                                                      \
	COND_CODE_1(AD74416H_IS_CH(node),                                                          \
		    ([DT_REG_ADDR(node)] = {                                                       \
		.func = DT_PROP(node, adi_ch_func),                                                \
		.vout_bipolar = DT_PROP(node, adi_vout_bipolar),                                   \
		.vout_4wire = DT_PROP(node, adi_vout_4wire),                                       \
		.i_limit_8ma = DT_PROP(node, adi_i_limit_8ma),                                     \
		.alarm_deglitch_20ms = DT_PROP(node, adi_alarm_deglitch_20ms),                     \
		.viout_drv_en_dly_200us = DT_PROP(node, adi_viout_drv_en_dly_200us),               \
		.avdd_select = DT_PROP_OR(node, adi_avdd_select, -1),                              \
		.slew = DT_PROP(node, adi_slew),                                                   \
		.slew_step = DT_PROP(node, adi_slew_step),                                         \
		.slew_rate = DT_PROP(node, adi_slew_rate),                                         \
		.dac_code = DT_PROP_OR(node, adi_dac_code, -1),                                    \
		.adc_range = DT_PROP_OR(node, adi_adc_range, -1),                                  \
		.adc_mux = DT_PROP_OR(node, adi_adc_mux, -1),                                      \
		.adc_rate = DT_PROP(node, adi_adc_rate),                                           \
		.rtd_2wire = DT_PROP(node, adi_rtd_2wire),                                         \
		.rtd_500ua = DT_PROP(node, adi_rtd_500ua),                                         \
		.rtd_ref_1v = DT_PROP(node, adi_rtd_ref_1v),                                       \
		.rtd_exc_swap = DT_PROP(node, adi_rtd_exc_swap),                                   \
		.din_comparator = DT_PROP_OR(node, adi_din_comparator, -1),                        \
		.din_invert = DT_PROP_OR(node, adi_din_invert, -1),                                \
		.din_threshold = DT_PROP_OR(node, adi_din_threshold, -1),                          \
		.din_threshold_fixed = DT_PROP(node, adi_din_threshold_fixed),                     \
		.din_sink = DT_PROP(node, adi_din_sink),                                           \
		.din_sink_range_high = DT_PROP(node, adi_din_sink_range_high),                     \
		.din_debounce_time = DT_PROP(node, adi_din_debounce_time),                         \
		.din_debounce_mode1 = DT_PROP(node, adi_din_debounce_mode1),                       \
		.din_counter = DT_PROP(node, adi_din_counter),                                     \
		.din_oc_detect = DT_PROP(node, adi_din_oc_detect),                                 \
		.din_sc_detect = DT_PROP(node, adi_din_sc_detect),                                 \
		.din_input_viout = DT_PROP(node, adi_din_input_viout),                             \
		.do_enable = DT_PROP(node, adi_do_enable),                                         \
		.do_gpio_control = DT_PROP(node, adi_do_gpio_control),                             \
		.do_t1 = DT_PROP(node, adi_do_t1),                                                 \
		.do_t2 = DT_PROP(node, adi_do_t2),                                                 \
		.fet_leakage_comp = DT_PROP(node, adi_fet_leakage_comp),                           \
		.burnout_viout = DT_PROP(node, adi_burnout_viout),                                 \
		.burnout_viout_source = DT_PROP(node, adi_burnout_viout_source),                   \
		.burnout_vsensen = DT_PROP(node, adi_burnout_vsensen),                             \
		.burnout_vsensen_source = DT_PROP(node, adi_burnout_vsensen_source),               \
	}, ),                                                                                      \
		    ())

#define AD74416H_CH_BIT(node) COND_CODE_1(AD74416H_IS_CH(node), (BIT(DT_REG_ADDR(node)) |), ())

#define AD74416H_CH_MASK(n) (DT_INST_FOREACH_CHILD_STATUS_OKAY(n, AD74416H_CH_BIT) 0)

#define AD74416H_CH_CFGS(n) DT_INST_FOREACH_CHILD_STATUS_OKAY(n, AD74416H_CH_CFG)

#define AD74416H_SPI_OP                                                                            \
	(SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL)

#define AD74416H_INIT(n)                                                                           \
	BUILD_ASSERT(DT_INST_PROP_LEN(n, adi_diag_assign) == AD74416H_NUM_DIAGS,                   \
		     "adi,diag-assign needs 4 entries");                                           \
                                                                                                   \
	static const struct ad74416h_config ad74416h_config_##n = {                                \
		.spi = SPI_DT_SPEC_INST_GET(n, AD74416H_SPI_OP),                                   \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),                       \
		.alert_gpio = GPIO_DT_SPEC_INST_GET_OR(n, alert_gpios, {0}),                       \
		.adc_rdy_gpio = GPIO_DT_SPEC_INST_GET_OR(n, adc_rdy_gpios, {0}),                   \
		.dev_addr = DT_INST_PROP(n, adi_dev_addr),                                         \
		.internal_ref = DT_INST_PROP(n, adi_internal_ref),                                 \
		.thermal_reset = DT_INST_PROP(n, adi_thermal_reset),                               \
		.diag_assign = DT_INST_PROP(n, adi_diag_assign),                                   \
		.diag_rate = DT_INST_PROP(n, adi_diag_rate),                                       \
		.ch_mask = AD74416H_CH_MASK(n),                                                    \
		.ch = {AD74416H_CH_CFGS(n)},                                                       \
	};                                                                                         \
                                                                                                   \
	static struct ad74416h_data ad74416h_data_##n;                                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, ad74416h_init, NULL, &ad74416h_data_##n, &ad74416h_config_##n,    \
			      POST_KERNEL, CONFIG_MFD_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(AD74416H_INIT)
