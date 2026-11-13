/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Register-level AD74416H emulator on an emulated SPI bus.
 *
 * Models the 40-bit CRC framing, two-stage readback, reset values (data sheet
 * Table 39 and Table 40), the defaults loaded by CH_FUNC_SETUP (Table 22),
 * software reset and DAC update keys, write-1-to-clear alerts, ADC
 * sequences, digital I/O, HART alert registers, and the ALERT and ADC_RDY pins. It also
 * counts violations of the channel function switching rules.
 */

#define DT_DRV_COMPAT adi_ad74416h

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/mfd/ad74416h.h>
#include "mfd_ad74416h_emul.h"

LOG_MODULE_REGISTER(mfd_ad74416h_emul, CONFIG_MFD_LOG_LEVEL);

#define FRAME_LEN 5
#define CRC_POLY  0x07
#define NUM_CH    AD74416H_NUM_CHANNELS

struct ad74416h_emul_cfg {
	uint8_t dev_addr;
	struct gpio_dt_spec alert_gpio;
	struct gpio_dt_spec adc_rdy_gpio;
};

struct ad74416h_emul_data {
	uint16_t regs[256];
	uint16_t readback;
	uint32_t adc_codes[8];
	bool adc_hang;
	bool cal_mem_fault;
	uint16_t last_key;
	uint16_t last_bcast_key;
	uint8_t gpi;
	uint32_t din_counter[NUM_CH];
	uint8_t seq_count;
	int64_t func_time[NUM_CH];
	struct ad74416h_emul_stats stats;
};

/* Channel register: returns the channel, or -1, and the base register */
static int ch_reg(uint8_t reg, uint8_t *base)
{
	if (reg < AD74416H_REG_CH_FUNC_SETUP(0) || reg > AD74416H_REG_DAC_ACTIVE(NUM_CH - 1)) {
		return -1;
	}

	*base = AD74416H_REG_CH_FUNC_SETUP(0) + (reg - 1) % 12;
	return (reg - 1) / 12;
}

static int hart_reg(uint8_t reg, uint8_t *base)
{
	if (reg < AD74416H_REG_HART_ALERT_STATUS(0) ||
	    reg > AD74416H_REG_HART_CH_RESET(NUM_CH - 1)) {
		return -1;
	}

	*base = AD74416H_REG_HART_ALERT_STATUS(0) + (reg & 0xF);
	return (reg - AD74416H_REG_HART_ALERT_STATUS(0)) / 16;
}

static void hart_defaults(struct ad74416h_emul_data *data, int ch)
{
	uint16_t *r = data->regs;

	for (int i = 0; i < 16; i++) {
		r[AD74416H_REG_HART_ALERT_STATUS(ch) + i] = 0;
	}
	r[AD74416H_REG_HART_ALERT_STATUS(ch)] = 0x0020;
	r[AD74416H_REG_HART_FCR(ch)] = 0x08C1;
	r[AD74416H_REG_HART_ALERT_MASK(ch)] = 0x1EFF;
	r[AD74416H_REG_HART_CONFIG(ch)] = 0x0C30;
	r[AD74416H_REG_HART_TX_PREM(ch)] = 0x0005;
}

static void reset_defaults(struct ad74416h_emul_data *data)
{
	uint16_t *r = data->regs;

	memset(r, 0, sizeof(data->regs));
	for (int ch = 0; ch < NUM_CH; ch++) {
		r[AD74416H_REG_ADC_CONFIG(ch)] = 0x0100;
		r[AD74416H_REG_DIN_CONFIG0(ch)] = 0x000B;
		r[AD74416H_REG_DIN_CONFIG1(ch)] = 0x0049;
		r[AD74416H_REG_OUTPUT_CONFIG(ch)] = 0x0100;
		r[AD74416H_REG_RTD_CONFIG(ch)] = 0x0001;
		r[AD74416H_REG_DO_EXT_CONFIG(ch)] = 0x1700;
		hart_defaults(data, ch);
		data->din_counter[ch] = 0;
		data->func_time[ch] = INT64_MIN / 2;
	}
	for (int i = 0; i < AD74416H_NUM_GPIOS; i++) {
		r[AD74416H_REG_GPIO_CONFIG(i)] = 0x0008;
	}
	r[AD74416H_REG_WDT_CONFIG] = 0x0009;
	r[AD74416H_REG_ALERT_STATUS] = AD74416H_ALERT_RESET_OCCURRED;
	r[AD74416H_REG_BURST_READ_SEL] = 0xFFFF;
	r[AD74416H_REG_GENERIC_ID] = AD74416H_GENERIC_ID_VALUE;
	r[AD74416H_REG_SILICON_REV] = 0x0002;
	r[AD74416H_REG_SILICON_ID0] = 0x0055;
	r[AD74416H_REG_SILICON_ID1] = 0x0ABC;
	if (data->cal_mem_fault) {
		r[AD74416H_REG_SUPPLY_ALERT_STATUS] = AD74416H_SUPPLY_CAL_MEM_ERR;
	}
	data->readback = 0;
	data->seq_count = 0;
}

/* Recompute the summary bits of ALERT_STATUS and drive the pins */
static void alerts_update(const struct emul *target)
{
	const struct ad74416h_emul_cfg *cfg = target->cfg;
	struct ad74416h_emul_data *data = target->data;
	uint16_t *r = data->regs;
	uint16_t alert = r[AD74416H_REG_ALERT_STATUS] &
			 (AD74416H_ALERT_ADC_ERR | AD74416H_ALERT_TEMP | AD74416H_ALERT_SPI_ERR |
			  AD74416H_ALERT_RESET_OCCURRED);
	uint16_t hart_relevant =
		AD74416H_HART_ALERT_W1C | AD74416H_HART_TX_FIFO_ALERT | AD74416H_HART_RX_FIFO_ALERT;

	for (int ch = 0; ch < NUM_CH; ch++) {
		if ((r[AD74416H_REG_CHANNEL_ALERT_STATUS(ch)] &
		     ~r[AD74416H_REG_CHANNEL_ALERT_MASK(ch)] & AD74416H_CH_ALERT_ALL) != 0) {
			alert |= AD74416H_ALERT_CHANNEL(ch);
		}
		if ((r[AD74416H_REG_HART_ALERT_STATUS(ch)] & ~r[AD74416H_REG_HART_ALERT_MASK(ch)] &
		     hart_relevant) != 0) {
			alert |= AD74416H_ALERT_HART(ch);
		}
	}
	if ((r[AD74416H_REG_SUPPLY_ALERT_STATUS] & ~r[AD74416H_REG_SUPPLY_ALERT_MASK] &
	     AD74416H_SUPPLY_ALL) != 0) {
		alert |= AD74416H_ALERT_SUPPLY_ERR;
	}

	r[AD74416H_REG_ALERT_STATUS] = alert;

	if (cfg->alert_gpio.port != NULL) {
		/* RESET_OCCURRED has no mask bit */
		bool active = (alert & ~r[AD74416H_REG_ALERT_MASK]) != 0 ||
			      (alert & AD74416H_ALERT_RESET_OCCURRED) != 0;
		bool active_low = (cfg->alert_gpio.dt_flags & GPIO_ACTIVE_LOW) != 0;

		(void)gpio_emul_input_set(cfg->alert_gpio.port, cfg->alert_gpio.pin,
					  active != active_low);
	}
}

static void adc_rdy_set(const struct emul *target, bool active)
{
	const struct ad74416h_emul_cfg *cfg = target->cfg;

	if (cfg->adc_rdy_gpio.port != NULL) {
		bool active_low = (cfg->adc_rdy_gpio.dt_flags & GPIO_ACTIVE_LOW) != 0;

		(void)gpio_emul_input_set(cfg->adc_rdy_gpio.port, cfg->adc_rdy_gpio.pin,
					  active != active_low);
	}
}

static void func_defaults(struct ad74416h_emul_data *data, int ch, uint8_t func)
{
	uint16_t *r = data->regs;
	uint8_t mux = AD74416H_ADC_MUX_LF_TO_AGND;
	uint8_t range = AD74416H_ADC_RANGE_0_12V;
	bool comparator = false;
	bool invert = false;
	uint8_t avdd = AD74416H_AVDD_HI;

	switch (func) {
	case AD74416H_CH_FUNC_VOUT:
		mux = AD74416H_ADC_MUX_HF_TO_LF;
		range = AD74416H_ADC_RANGE_PM_312MV;
		break;
	case AD74416H_CH_FUNC_IOUT:
	case AD74416H_CH_FUNC_IOUT_HART:
		avdd = AD74416H_AVDD_TRACK;
		break;
	case AD74416H_CH_FUNC_IIN_EXT_PWR:
	case AD74416H_CH_FUNC_IIN_EXT_PWR_HART:
		mux = AD74416H_ADC_MUX_HF_TO_LF;
		range = AD74416H_ADC_RANGE_NEG_312MV_0;
		comparator = true;
		break;
	case AD74416H_CH_FUNC_IIN_LOOP_PWR:
	case AD74416H_CH_FUNC_IIN_LOOP_PWR_HART:
		mux = AD74416H_ADC_MUX_HF_TO_LF;
		range = AD74416H_ADC_RANGE_0_312MV;
		comparator = true;
		invert = true;
		break;
	case AD74416H_CH_FUNC_RES_MEAS:
		mux = AD74416H_ADC_MUX_LF_TO_VSENSEN;
		range = AD74416H_ADC_RANGE_0_625MV;
		break;
	case AD74416H_CH_FUNC_DIN_LOGIC:
	case AD74416H_CH_FUNC_DIN_LOOP:
		comparator = true;
		break;
	default:
		break;
	}

	r[AD74416H_REG_ADC_CONFIG(ch)] = (r[AD74416H_REG_ADC_CONFIG(ch)] & AD74416H_ADC_CONV_RATE) |
					 FIELD_PREP(AD74416H_ADC_CONV_RANGE, range) |
					 FIELD_PREP(AD74416H_ADC_CONV_MUX, mux);
	r[AD74416H_REG_DIN_CONFIG0(ch)] =
		(r[AD74416H_REG_DIN_CONFIG0(ch)] &
		 ~(AD74416H_DIN_COMPARATOR_EN | AD74416H_DIN_INV_COMP_OUT | AD74416H_DIN_SINK)) |
		FIELD_PREP(AD74416H_DIN_COMPARATOR_EN, comparator) |
		FIELD_PREP(AD74416H_DIN_INV_COMP_OUT, invert);
	r[AD74416H_REG_DIN_CONFIG1(ch)] =
		(r[AD74416H_REG_DIN_CONFIG1(ch)] &
		 ~(AD74416H_DIN_COMP_THRESH | AD74416H_DIN_THRESH_MODE)) |
		FIELD_PREP(AD74416H_DIN_COMP_THRESH, comparator ? 0x49 : 0);
	r[AD74416H_REG_OUTPUT_CONFIG(ch)] =
		(r[AD74416H_REG_OUTPUT_CONFIG(ch)] & ~AD74416H_OUT_AVDD_SELECT) |
		FIELD_PREP(AD74416H_OUT_AVDD_SELECT, avdd);
	r[AD74416H_REG_RTD_CONFIG(ch)] = AD74416H_RTD_CURRENT;
}

static void ch_func_write(struct ad74416h_emul_data *data, int ch, uint16_t val)
{
	uint16_t *r = data->regs;
	uint8_t old = FIELD_GET(AD74416H_CH_FUNC, r[AD74416H_REG_CH_FUNC_SETUP(ch)]);
	uint8_t func = FIELD_GET(AD74416H_CH_FUNC, val);
	int64_t now = k_ticks_to_us_floor64(k_uptime_ticks());
	uint32_t min_us = old == AD74416H_CH_FUNC_IOUT_HART ? AD74416H_CH_INIT_TIME_HART_US
							    : AD74416H_CH_INIT_TIME_US;
	struct ad74416h_emul_stats *s = &data->stats;

	if (func == 0x6 || func > AD74416H_CH_FUNC_IIN_LOOP_PWR_HART) {
		func = AD74416H_CH_FUNC_HIGH_IMP;
	}

	if (old != AD74416H_CH_FUNC_HIGH_IMP && func != AD74416H_CH_FUNC_HIGH_IMP && old != func) {
		LOG_WRN("Ch %d: function %u -> %u without high impedance", ch, old, func);
		s->violations++;
	}
	if (old != func && now - data->func_time[ch] < min_us) {
		LOG_WRN("Ch %d: function %u held %lld us", ch, old, now - data->func_time[ch]);
		s->violations++;
	}

	if (s->func_history_len[ch] < AD74416H_EMUL_FUNC_HISTORY) {
		s->func_history[ch][s->func_history_len[ch]++] = func;
	}

	data->func_time[ch] = now;
	r[AD74416H_REG_CH_FUNC_SETUP(ch)] = func;
	func_defaults(data, ch, func);
}

static void adc_sequence(const struct emul *target, uint16_t ctrl)
{
	struct ad74416h_emul_data *data = target->data;
	uint16_t *r = data->regs;
	uint8_t seq = FIELD_GET(AD74416H_CONV_SEQ, ctrl);

	adc_rdy_set(target, false);
	r[AD74416H_REG_LIVE_STATUS] &= ~(AD74416H_LIVE_ADC_DATA_RDY | AD74416H_LIVE_ADC_BUSY);

	if (seq != AD74416H_CONV_SEQ_SINGLE && seq != AD74416H_CONV_SEQ_CONTINUOUS) {
		return;
	}

	if (data->adc_hang) {
		r[AD74416H_REG_LIVE_STATUS] |= AD74416H_LIVE_ADC_BUSY;
		return;
	}

	data->seq_count = (data->seq_count + 1) & 0x3;

	for (int ch = 0; ch < NUM_CH; ch++) {
		uint16_t cfg = r[AD74416H_REG_ADC_CONFIG(ch)];

		if ((ctrl & AD74416H_CONV_CH_EN(ch)) == 0) {
			continue;
		}
		r[AD74416H_REG_ADC_RESULT_UPR(ch)] =
			FIELD_PREP(AD74416H_RES_CONV_RES_MUX,
				   FIELD_GET(AD74416H_ADC_CONV_MUX, cfg)) |
			FIELD_PREP(AD74416H_RES_CONV_RES_RANGE,
				   FIELD_GET(AD74416H_ADC_CONV_RANGE, cfg)) |
			FIELD_PREP(AD74416H_RES_CONV_SEQ_COUNT, data->seq_count) |
			FIELD_PREP(AD74416H_RES_CONV_RES_UPR, data->adc_codes[ch] >> 16);
		r[AD74416H_REG_ADC_RESULT(ch)] = data->adc_codes[ch] & 0xFFFF;
	}
	for (int d = 0; d < AD74416H_NUM_DIAGS; d++) {
		if ((ctrl & AD74416H_CONV_DIAG_EN(d)) != 0) {
			r[AD74416H_REG_ADC_DIAG_RESULT(d)] = data->adc_codes[4 + d] & 0xFFFF;
		}
	}

	r[AD74416H_REG_LIVE_STATUS] |= AD74416H_LIVE_ADC_DATA_RDY;
	adc_rdy_set(target, true);
}

static void hart_write(struct ad74416h_emul_data *data, int ch, uint8_t base, uint16_t val)
{
	uint16_t *r = data->regs;
	uint8_t reg = base + 16 * ch;

	switch (base) {
	case AD74416H_REG_HART_ALERT_STATUS(0):
		r[reg] &= ~(val & AD74416H_HART_ALERT_W1C);
		break;
	case AD74416H_REG_HART_FCR(0):
		r[reg] = val & (AD74416H_HART_TFTRIG | AD74416H_HART_RFTRIG | AD74416H_HART_FIFOEN);
		break;
	case AD74416H_REG_HART_MCR(0):
		r[reg] = val & AD74416H_HART_RTS;
		break;
	case AD74416H_REG_HART_CH_RESET(0):
		if ((val & AD74416H_HART_HRST) != 0) {
			hart_defaults(data, ch);
		}
		r[reg] = val & AD74416H_HART_HRST;
		break;
	case AD74416H_REG_HART_ALERT_MASK(0):
	case AD74416H_REG_HART_CONFIG(0):
	case AD74416H_REG_HART_TX_PREM(0):
		r[reg] = val;
		break;
	default:
		/* HART_RX, HART_RFC, HART_TFC, HART_EVDET are read only */
		break;
	}
}

static void reg_write(const struct emul *target, uint8_t reg, uint16_t val)
{
	struct ad74416h_emul_data *data = target->data;
	uint16_t *r = data->regs;
	uint8_t base;
	int ch;

	if (reg != AD74416H_REG_CMD_KEY) {
		data->last_key = 0;
	}
	if (reg != AD74416H_REG_BROADCAST_CMD_KEY) {
		data->last_bcast_key = 0;
	}

	ch = ch_reg(reg, &base);
	if (ch >= 0) {
		switch (base) {
		case AD74416H_REG_CH_FUNC_SETUP(0):
			ch_func_write(data, ch, val);
			break;
		case AD74416H_REG_OUTPUT_CONFIG(0): {
			uint16_t keep_hiz = AD74416H_OUT_VOUT_RANGE | AD74416H_OUT_VIOUT_DRV_EN_DLY;

			if (FIELD_GET(AD74416H_CH_FUNC, r[AD74416H_REG_CH_FUNC_SETUP(ch)]) !=
				    AD74416H_CH_FUNC_HIGH_IMP &&
			    ((r[reg] ^ val) & keep_hiz) != 0) {
				data->stats.violations++;
				r[AD74416H_REG_CHANNEL_ALERT_STATUS(ch)] |=
					AD74416H_CH_ALERT_VIOUT_SHUTDOWN;
			}
			r[reg] = (val & ~AD74416H_OUT_HART_COMPL_SETTLED) |
				 (r[reg] & AD74416H_OUT_HART_COMPL_SETTLED);
			break;
		}
		case AD74416H_REG_DAC_CODE(0): {
			uint16_t out = r[AD74416H_REG_OUTPUT_CONFIG(ch)];

			r[reg] = val;
			/* Linear slewing completes with ad74416h_emul_dac_settle() */
			if ((out & AD74416H_OUT_WAIT_LDAC_CMD) == 0 &&
			    FIELD_GET(AD74416H_OUT_SLEW_EN, out) != AD74416H_SLEW_LINEAR) {
				r[AD74416H_REG_DAC_ACTIVE(ch)] = val;
			}
			break;
		}
		case AD74416H_REG_DAC_ACTIVE(0):
			break;
		case AD74416H_REG_DO_EXT_CONFIG(0):
			r[reg] = val & GENMASK(12, 0);
			break;
		default:
			r[reg] = val;
			break;
		}
		return;
	}

	ch = hart_reg(reg, &base);
	if (ch >= 0) {
		hart_write(data, ch, base, val);
		return;
	}

	switch (reg) {
	case AD74416H_REG_GPIO_CONFIG(0)... AD74416H_REG_GPIO_CONFIG(5):
		r[reg] = (val & ~AD74416H_GPIO_GPI_DATA) | (r[reg] & AD74416H_GPIO_GPI_DATA);
		break;
	case AD74416H_REG_ADC_CONV_CTRL:
		r[reg] = val & GENMASK(13, 0);
		adc_sequence(target, val);
		break;
	case AD74416H_REG_PWR_OPTIM_CONFIG:
	case AD74416H_REG_DIAG_ASSIGN:
	case AD74416H_REG_WDT_CONFIG:
	case AD74416H_REG_ALERT_MASK:
	case AD74416H_REG_SUPPLY_ALERT_MASK:
	case AD74416H_REG_CHANNEL_ALERT_MASK(0)... AD74416H_REG_CHANNEL_ALERT_MASK(3):
	case AD74416H_REG_BURST_READ_SEL:
	case AD74416H_REG_THERM_RST:
	case AD74416H_REG_SCRATCH(0)... AD74416H_REG_SCRATCH(3):
	case AD74416H_REG_HART_GPIO_IF_CONFIG:
	case AD74416H_REG_HART_GPIO_MON_CONFIG(0)... AD74416H_REG_HART_GPIO_MON_CONFIG(3):
		r[reg] = val;
		break;
	case AD74416H_REG_ALERT_STATUS:
		r[reg] &=
			~(val & (AD74416H_ALERT_ADC_ERR | AD74416H_ALERT_TEMP |
				 AD74416H_ALERT_SPI_ERR | BIT(1) | AD74416H_ALERT_RESET_OCCURRED));
		break;
	case AD74416H_REG_SUPPLY_ALERT_STATUS:
		r[reg] &= ~(val & AD74416H_SUPPLY_ALL);
		if (data->cal_mem_fault) {
			r[reg] |= AD74416H_SUPPLY_CAL_MEM_ERR;
		}
		break;
	case AD74416H_REG_CHANNEL_ALERT_STATUS(0)... AD74416H_REG_CHANNEL_ALERT_STATUS(3):
		r[reg] &= ~(val & AD74416H_CH_ALERT_ALL);
		break;
	case AD74416H_REG_READ_SELECT:
		data->readback = val & AD74416H_READBACK_ADDR;
		r[reg] = data->readback;
		break;
	case AD74416H_REG_CMD_KEY:
		if (data->last_key == AD74416H_CMD_SW_RESET_KEY1 &&
		    val == AD74416H_CMD_SW_RESET_KEY2) {
			reset_defaults(data);
			data->stats.resets++;
		} else if (val == AD74416H_CMD_DAC_UPDATE) {
			for (int c = 0; c < NUM_CH; c++) {
				r[AD74416H_REG_DAC_ACTIVE(c)] = r[AD74416H_REG_DAC_CODE(c)];
			}
		}
		data->last_key = val;
		break;
	case AD74416H_REG_BROADCAST_CMD_KEY:
		if (data->last_bcast_key == AD74416H_BCAST_SW_RESET_KEY1 &&
		    val == AD74416H_BCAST_SW_RESET_KEY2) {
			reset_defaults(data);
			data->stats.resets++;
		}
		data->last_bcast_key = val;
		break;
	default:
		/* Read only or unused */
		break;
	}
}

/* Value of the read-back register, with read side effects */
static uint16_t reg_read(struct ad74416h_emul_data *data, uint16_t addr)
{
	uint16_t *r = data->regs;

	if (addr > UINT8_MAX) {
		return 0;
	}

	if (addr >= AD74416H_REG_DIN_COUNTER_UPR(0) &&
	    addr <= AD74416H_REG_DIN_COUNTER(NUM_CH - 1)) {
		uint32_t count = data->din_counter[(addr - AD74416H_REG_DIN_COUNTER_UPR(0)) / 2];

		return (addr & 1) != 0 ? count >> 16 : count & 0xFFFF;
	}

	/* Reading results clears ADC_DATA_RDY */
	if (addr >= AD74416H_REG_ADC_RESULT_UPR(0) &&
	    addr <= AD74416H_REG_ADC_DIAG_RESULT(AD74416H_NUM_DIAGS - 1)) {
		r[AD74416H_REG_LIVE_STATUS] &= ~AD74416H_LIVE_ADC_DATA_RDY;
	}

	return r[addr];
}

static int ad74416h_emul_io(const struct emul *target, const struct spi_config *config,
			    const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	const struct ad74416h_emul_cfg *cfg = target->cfg;
	struct ad74416h_emul_data *data = target->data;
	uint8_t tx[FRAME_LEN];
	uint8_t rx[FRAME_LEN];
	size_t len = 0;
	uint16_t rb_val;

	ARG_UNUSED(config);

	if (tx_bufs == NULL) {
		return -EINVAL;
	}

	for (size_t i = 0; i < tx_bufs->count; i++) {
		const struct spi_buf *b = &tx_bufs->buffers[i];

		if (len + b->len > FRAME_LEN) {
			len = SIZE_MAX;
			break;
		}
		memcpy(&tx[len], b->buf, b->len);
		len += b->len;
	}

	if (len != FRAME_LEN) {
		/* Not 40 clocks: ignored and flagged */
		data->stats.bad_frames++;
		data->regs[AD74416H_REG_ALERT_STATUS] |= AD74416H_ALERT_SPI_ERR;
		alerts_update(target);
		return 0;
	}

	/* The read stage shifts out the register selected by READ_SELECT */
	alerts_update(target);
	rb_val = reg_read(data, data->readback);
	rx[0] = BIT(7) | FIELD_PREP(GENMASK(5, 4), cfg->dev_addr) |
		FIELD_PREP(BIT(3), data->regs[AD74416H_REG_ALERT_STATUS] != 0) |
		FIELD_PREP(BIT(2), (data->regs[AD74416H_REG_ALERT_STATUS] & GENMASK(15, 12)) != 0) |
		FIELD_PREP(BIT(1), (data->regs[AD74416H_REG_ALERT_STATUS] & GENMASK(11, 8)) != 0) |
		FIELD_PREP(BIT(0), (data->regs[AD74416H_REG_LIVE_STATUS] &
				    AD74416H_LIVE_ADC_DATA_RDY) != 0);
	rx[1] = data->readback & 0xFF;
	sys_put_be16(rb_val, &rx[2]);
	rx[4] = crc8(rx, 4, CRC_POLY, 0, false);

	if (rx_bufs != NULL) {
		size_t off = 0;

		for (size_t i = 0; i < rx_bufs->count && off < FRAME_LEN; i++) {
			const struct spi_buf *b = &rx_bufs->buffers[i];
			size_t n = MIN(b->len, FRAME_LEN - off);

			if (b->buf != NULL) {
				memcpy(b->buf, &rx[off], n);
			}
			off += n;
		}
	}

	if (crc8(tx, 4, CRC_POLY, 0, false) != tx[4] || (tx[0] & GENMASK(7, 6)) != 0) {
		data->stats.bad_frames++;
		data->regs[AD74416H_REG_ALERT_STATUS] |= AD74416H_ALERT_SPI_ERR;
		alerts_update(target);
		return 0;
	}

	data->stats.frames++;

	if (FIELD_GET(GENMASK(5, 4), tx[0]) == cfg->dev_addr) {
		reg_write(target, tx[1], sys_get_be16(&tx[2]));
	}

	/* GPI_DATA follows the pin, or the output in GPO mode */
	for (int i = 0; i < AD74416H_NUM_GPIOS; i++) {
		uint16_t *g = &data->regs[AD74416H_REG_GPIO_CONFIG(i)];
		bool level = FIELD_GET(AD74416H_GPIO_SELECT, *g) == AD74416H_GPIO_SEL_GPO
				     ? (*g & AD74416H_GPIO_GPO_DATA) != 0
				     : (data->gpi & BIT(i)) != 0;

		WRITE_BIT(*g, 5, level);
	}

	alerts_update(target);
	return 0;
}

/* Backdoor API */

uint16_t ad74416h_emul_reg_get(const struct emul *target, uint8_t reg)
{
	struct ad74416h_emul_data *data = target->data;

	return data->regs[reg];
}

void ad74416h_emul_reg_set(const struct emul *target, uint8_t reg, uint16_t val)
{
	struct ad74416h_emul_data *data = target->data;

	data->regs[reg] = val;
}

void ad74416h_emul_adc_code_set(const struct emul *target, uint8_t index, uint32_t code)
{
	struct ad74416h_emul_data *data = target->data;

	if (index < ARRAY_SIZE(data->adc_codes)) {
		data->adc_codes[index] = code;
	}
}

void ad74416h_emul_adc_hang(const struct emul *target, bool hang)
{
	struct ad74416h_emul_data *data = target->data;

	data->adc_hang = hang;
	if (!hang) {
		data->regs[AD74416H_REG_LIVE_STATUS] &= ~AD74416H_LIVE_ADC_BUSY;
	}
}

void ad74416h_emul_gpi_set(const struct emul *target, uint8_t pin, bool level)
{
	struct ad74416h_emul_data *data = target->data;

	WRITE_BIT(data->gpi, pin, level);
	if (FIELD_GET(AD74416H_GPIO_SELECT, data->regs[AD74416H_REG_GPIO_CONFIG(pin)]) !=
	    AD74416H_GPIO_SEL_GPO) {
		WRITE_BIT(data->regs[AD74416H_REG_GPIO_CONFIG(pin)], 5, level);
	}
}

void ad74416h_emul_din_set(const struct emul *target, uint8_t state)
{
	struct ad74416h_emul_data *data = target->data;

	data->regs[AD74416H_REG_DIN_COMP_OUT] = state & 0xF;
}

void ad74416h_emul_din_counter_set(const struct emul *target, uint8_t ch, uint32_t count)
{
	struct ad74416h_emul_data *data = target->data;

	data->din_counter[ch] = count;
}

void ad74416h_emul_dac_settle(const struct emul *target, uint8_t ch)
{
	struct ad74416h_emul_data *data = target->data;

	data->regs[AD74416H_REG_DAC_ACTIVE(ch)] = data->regs[AD74416H_REG_DAC_CODE(ch)];
}

void ad74416h_emul_channel_alert_set(const struct emul *target, uint8_t ch, uint16_t bits)
{
	struct ad74416h_emul_data *data = target->data;

	data->regs[AD74416H_REG_CHANNEL_ALERT_STATUS(ch)] |= bits & AD74416H_CH_ALERT_ALL;
	alerts_update(target);
}

void ad74416h_emul_supply_alert_set(const struct emul *target, uint16_t bits)
{
	struct ad74416h_emul_data *data = target->data;

	data->regs[AD74416H_REG_SUPPLY_ALERT_STATUS] |= bits & AD74416H_SUPPLY_ALL;
	alerts_update(target);
}

void ad74416h_emul_cal_mem_fault(const struct emul *target, bool fault)
{
	struct ad74416h_emul_data *data = target->data;

	data->cal_mem_fault = fault;
}

const struct ad74416h_emul_stats *ad74416h_emul_stats_get(const struct emul *target)
{
	struct ad74416h_emul_data *data = target->data;

	return &data->stats;
}

void ad74416h_emul_stats_clear(const struct emul *target)
{
	struct ad74416h_emul_data *data = target->data;

	memset(&data->stats, 0, sizeof(data->stats));
}

static int ad74416h_emul_init(const struct emul *target, const struct device *parent)
{
	struct ad74416h_emul_data *data = target->data;

	ARG_UNUSED(parent);

	reset_defaults(data);
	return 0;
}

static struct spi_emul_api ad74416h_emul_spi_api = {
	.io = ad74416h_emul_io,
};

#define AD74416H_EMUL(n)                                                                           \
	static const struct ad74416h_emul_cfg ad74416h_emul_cfg_##n = {                            \
		.dev_addr = DT_INST_PROP(n, adi_dev_addr),                                         \
		.alert_gpio = GPIO_DT_SPEC_INST_GET_OR(n, alert_gpios, {0}),                       \
		.adc_rdy_gpio = GPIO_DT_SPEC_INST_GET_OR(n, adc_rdy_gpios, {0}),                   \
	};                                                                                         \
	static struct ad74416h_emul_data ad74416h_emul_data_##n;                                   \
	EMUL_DT_INST_DEFINE(n, ad74416h_emul_init, &ad74416h_emul_data_##n,                        \
			    &ad74416h_emul_cfg_##n, &ad74416h_emul_spi_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(AD74416H_EMUL)
