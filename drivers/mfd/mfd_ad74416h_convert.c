/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AD74416H unit conversions, from the transfer functions in the data sheet
 * (Table 24, Table 30, Table 31 and the RTD and DIN threshold equations).
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zephyr/drivers/mfd/ad74416h.h>

#define ADC_FULL_SCALE  (1LL << 24)
#define ADC_MIDSCALE    (1LL << 23)
#define DIAG_FULL_SCALE 65536LL

/* Rounded division of signed values by a positive divisor */
static int64_t div_round(int64_t num, int64_t den)
{
	return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

struct adc_range_info {
	int64_t zero_nv; /* Input voltage at code 0 */
	int64_t span_nv; /* Input voltage change from code 0 to full scale */
	bool bipolar;
	/* Full-scale magnitude relative to 2.5 V, as num/den, for the RTD gain */
	uint8_t fs_num;
	uint8_t fs_den;
};

static const struct adc_range_info adc_ranges[] = {
	[AD74416H_ADC_RANGE_0_12V] = {0, 12000000000LL, false, 24, 5},
	[AD74416H_ADC_RANGE_PM_12V] = {-12000000000LL, 24000000000LL, true, 24, 5},
	[AD74416H_ADC_RANGE_PM_312MV] = {-312500000LL, 625000000LL, true, 1, 8},
	/* Code 0 is 0 V and full scale is -0.3125 V */
	[AD74416H_ADC_RANGE_NEG_312MV_0] = {0, -312500000LL, false, 1, 8},
	[AD74416H_ADC_RANGE_0_312MV] = {0, 312500000LL, false, 1, 8},
	[AD74416H_ADC_RANGE_0_625MV] = {0, 625000000LL, false, 1, 4},
	/* +-2.5 V / 24 */
	[AD74416H_ADC_RANGE_PM_104MV] = {-104166667LL, 208333333LL, true, 1, 24},
	[AD74416H_ADC_RANGE_PM_2V5] = {-2500000000LL, 5000000000LL, true, 1, 1},
};

int64_t ad74416h_adc_code_to_nv(uint8_t range, uint32_t code)
{
	const struct adc_range_info *r;

	if (range >= ARRAY_SIZE(adc_ranges)) {
		return 0;
	}

	r = &adc_ranges[range];
	code &= (uint32_t)(ADC_FULL_SCALE - 1);

	return r->zero_nv + div_round(r->span_nv * (int64_t)code, ADC_FULL_SCALE);
}

int64_t ad74416h_adc_code_to_na(uint8_t range, uint32_t code, uint32_t rsense_mohm)
{
	if (rsense_mohm == 0) {
		return 0;
	}

	/* I[nA] = V[nV] / R[ohm] = V[nV] * 1000 / R[mohm] */
	return div_round(ad74416h_adc_code_to_nv(range, code) * 1000, rsense_mohm);
}

int64_t ad74416h_rtd_code_to_mohm(uint8_t range, uint32_t code)
{
	const struct adc_range_info *r;
	int64_t num;
	int64_t den;

	if (range >= ARRAY_SIZE(adc_ranges)) {
		return 0;
	}

	r = &adc_ranges[range];
	code &= (uint32_t)(ADC_FULL_SCALE - 1);

	/*
	 * Unipolar: R = code / (2^24 * gain) * RREF
	 * Bipolar:  R = (code - 2^23) / (2^23 * gain) * RREF
	 * with gain = 2.5 V / full-scale magnitude = fs_den / fs_num.
	 */
	if (r->bipolar) {
		num = ((int64_t)code - ADC_MIDSCALE) * AD74416H_RTD_RREF_MOHM * r->fs_num;
		den = ADC_MIDSCALE * r->fs_den;
	} else {
		num = (int64_t)code * AD74416H_RTD_RREF_MOHM * r->fs_num;
		den = ADC_FULL_SCALE * r->fs_den;
	}

	return div_round(num, den);
}

/* code / 65536 * scale_uv */
static int64_t diag_scale(uint16_t code, int64_t scale_num_uv, int64_t scale_den)
{
	return div_round((int64_t)code * scale_num_uv, DIAG_FULL_SCALE * scale_den);
}

int64_t ad74416h_diag_code_to_value(uint8_t diag, uint16_t code, int32_t aux)
{
	switch (diag) {
	case AD74416H_DIAG_AGND:
	case AD74416H_DIAG_LVIN:
		return diag_scale(code, 2500000, 1);
	case AD74416H_DIAG_TEMP:
		/* T = (code - 2034) / 8.95 - 40 degC */
		return div_round(((int64_t)code - 2034) * 100000, 895) - 40000;
	case AD74416H_DIAG_DVCC:
		return diag_scale(code, 25000000, 3);
	case AD74416H_DIAG_AVCC:
		return diag_scale(code, 17500000, 1);
	case AD74416H_DIAG_LDO1V8:
		return diag_scale(code, 7500000, 1);
	case AD74416H_DIAG_AVDD_HI:
	case AD74416H_DIAG_AVDD_LO:
	case AD74416H_DIAG_AVDD_CH:
		/* x 25 / 0.52 */
		return diag_scale(code, 2500000000LL, 52);
	case AD74416H_DIAG_AVSS:
		return diag_scale(code, 31017000, 1) - 20000000;
	case AD74416H_DIAG_DO_VDD:
		/* x 25 / 0.64 */
		return diag_scale(code, 2500000000LL, 64);
	case AD74416H_DIAG_VSENSEP:
		if (aux == INT32_MIN) {
			return diag_scale(code, 50000000, 1) - 20000000;
		}
		return diag_scale(code, 60000000, 1) - aux;
	case AD74416H_DIAG_VSENSEN:
		return diag_scale(code, 50000000, 1) - 20000000;
	case AD74416H_DIAG_I_DO_SRC:
		/* I = (code / 65536 * 0.5 V) / RSET; uA = uV * 1000 / mohm */
		if (aux <= 0) {
			return 0;
		}
		return div_round((int64_t)code * 500000 * 1000, DIAG_FULL_SCALE * aux);
	default:
		return 0;
	}
}

int ad74416h_dac_uv_to_code(bool bipolar, int32_t uv, uint16_t *code)
{
	int64_t c;

	if (bipolar) {
		if (uv < -12000000 || uv > 12000000) {
			return -ERANGE;
		}
		c = div_round(((int64_t)uv + 12000000) * 65536, 24000000);
	} else {
		if (uv < 0 || uv > 12000000) {
			return -ERANGE;
		}
		c = div_round((int64_t)uv * 65536, 12000000);
	}

	*code = MIN(c, UINT16_MAX);
	return 0;
}

int ad74416h_dac_ua_to_code(int32_t ua, uint16_t *code)
{
	if (ua < 0 || ua > 25000) {
		return -ERANGE;
	}

	*code = MIN(div_round((int64_t)ua * 65536, 25000), UINT16_MAX);
	return 0;
}

int ad74416h_din_threshold_fixed_mv_to_code(int32_t mv, uint8_t *code)
{
	/* V = 2.5 V * (code - 38) / 5 = 0.5 V * (code - 38) */
	int64_t c = div_round(mv, 500) + 38;

	if (c < 0 || c > AD74416H_DIN_COMP_THRESH_MAX) {
		return -ERANGE;
	}

	*code = c;
	return 0;
}

int ad74416h_din_threshold_avdd_mv_to_code(int32_t mv, int32_t avdd_hi_mv, uint8_t *code)
{
	int64_t c;

	if (avdd_hi_mv <= 0) {
		return -EINVAL;
	}

	/* V = AVDD_HI * (code - 48) / 50 */
	c = div_round((int64_t)mv * 50, avdd_hi_mv) + 48;
	if (c < 0 || c > AD74416H_DIN_COMP_THRESH_MAX) {
		return -ERANGE;
	}

	*code = c;
	return 0;
}
