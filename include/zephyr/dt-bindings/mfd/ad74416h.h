/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Devicetree constants for the Analog Devices AD74416H quad-channel
 * software configurable input/output. Values are the register field codes
 * from the AD74416H data sheet, Rev. 0.
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_MFD_AD74416H_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_MFD_AD74416H_H_

/* CH_FUNC_SETUPn.CH_FUNC: channel function */
#define AD74416H_CH_FUNC_HIGH_IMP          0x0
#define AD74416H_CH_FUNC_VOUT              0x1
#define AD74416H_CH_FUNC_IOUT              0x2
#define AD74416H_CH_FUNC_VIN               0x3
#define AD74416H_CH_FUNC_IIN_EXT_PWR       0x4
#define AD74416H_CH_FUNC_IIN_LOOP_PWR      0x5
#define AD74416H_CH_FUNC_RES_MEAS          0x7
#define AD74416H_CH_FUNC_DIN_LOGIC         0x8
#define AD74416H_CH_FUNC_DIN_LOOP          0x9
#define AD74416H_CH_FUNC_IOUT_HART         0xA
#define AD74416H_CH_FUNC_IIN_EXT_PWR_HART  0xB
#define AD74416H_CH_FUNC_IIN_LOOP_PWR_HART 0xC

/* ADC_CONFIGn.CONV_RANGE: ADC input range */
#define AD74416H_ADC_RANGE_0_12V       0x0
#define AD74416H_ADC_RANGE_PM_12V      0x1
#define AD74416H_ADC_RANGE_PM_312MV    0x2
#define AD74416H_ADC_RANGE_NEG_312MV_0 0x3
#define AD74416H_ADC_RANGE_0_312MV     0x4
#define AD74416H_ADC_RANGE_0_625MV     0x5
#define AD74416H_ADC_RANGE_PM_104MV    0x6
#define AD74416H_ADC_RANGE_PM_2V5      0x7

/* ADC_CONFIGn.CONV_RATE: channel conversion rate (_H: HART rejection) */
#define AD74416H_ADC_RATE_10SPS_H      0x0
#define AD74416H_ADC_RATE_20SPS        0x1
#define AD74416H_ADC_RATE_20SPS_H      0x3
#define AD74416H_ADC_RATE_200SPS_H_MOD 0x4
#define AD74416H_ADC_RATE_200SPS_H     0x6
#define AD74416H_ADC_RATE_1K2SPS       0x8
#define AD74416H_ADC_RATE_1K2SPS_H     0x9
#define AD74416H_ADC_RATE_4K8SPS       0xC
#define AD74416H_ADC_RATE_9K6SPS       0xD

/* ADC_CONFIGn.CONV_MUX: ADC input node */
#define AD74416H_ADC_MUX_LF_TO_AGND      0x0
#define AD74416H_ADC_MUX_HF_TO_LF        0x1
#define AD74416H_ADC_MUX_VSENSEN_TO_AGND 0x2
#define AD74416H_ADC_MUX_LF_TO_VSENSEN   0x3
#define AD74416H_ADC_MUX_AGND_TO_AGND    0x4

/* DIAG_ASSIGN.DIAGn: diagnostic source */
#define AD74416H_DIAG_AGND     0x0
#define AD74416H_DIAG_TEMP     0x1
#define AD74416H_DIAG_DVCC     0x2
#define AD74416H_DIAG_AVCC     0x3
#define AD74416H_DIAG_LDO1V8   0x4
#define AD74416H_DIAG_AVDD_HI  0x5
#define AD74416H_DIAG_AVDD_LO  0x6
#define AD74416H_DIAG_AVSS     0x7
#define AD74416H_DIAG_LVIN     0x8
#define AD74416H_DIAG_DO_VDD   0x9
#define AD74416H_DIAG_VSENSEP  0xA /* of the channel matching the diagnostic slot */
#define AD74416H_DIAG_VSENSEN  0xB /* of the channel matching the diagnostic slot */
#define AD74416H_DIAG_I_DO_SRC 0xC /* of the channel matching the diagnostic slot */
#define AD74416H_DIAG_AVDD_CH  0xD /* of the channel matching the diagnostic slot */

/* ADC_CONV_CTRL.CONV_RATE_DIAG: diagnostic conversion rate */
#define AD74416H_DIAG_RATE_20SPS    0x0
#define AD74416H_DIAG_RATE_20SPS_H  0x1
#define AD74416H_DIAG_RATE_1K2SPS_H 0x2
#define AD74416H_DIAG_RATE_4K8SPS   0x3
#define AD74416H_DIAG_RATE_9K6SPS   0x4
#define AD74416H_DIAG_RATE_19K2SPS  0x5

/* OUTPUT_CONFIGn.AVDD_SELECT: adaptive power switching */
#define AD74416H_AVDD_HI    0x0
#define AD74416H_AVDD_LO    0x1
#define AD74416H_AVDD_TRACK 0x2

/* OUTPUT_CONFIGn.SLEW_EN */
#define AD74416H_SLEW_OFF    0x0
#define AD74416H_SLEW_LINEAR 0x1
#define AD74416H_SLEW_HART   0x2

/* OUTPUT_CONFIGn.SLEW_LIN_STEP: step, % of full scale */
#define AD74416H_SLEW_STEP_0P8  0x0
#define AD74416H_SLEW_STEP_1P5  0x1
#define AD74416H_SLEW_STEP_6P1  0x2
#define AD74416H_SLEW_STEP_22P2 0x3

/* OUTPUT_CONFIGn.SLEW_LIN_RATE: update rate */
#define AD74416H_SLEW_RATE_4K8HZ   0x0
#define AD74416H_SLEW_RATE_76K8HZ  0x1
#define AD74416H_SLEW_RATE_153K6HZ 0x2
#define AD74416H_SLEW_RATE_230K4HZ 0x3

/* I_BURNOUT_CONFIGn: burnout current magnitude */
#define AD74416H_BURNOUT_OFF   0x0
#define AD74416H_BURNOUT_100NA 0x1
#define AD74416H_BURNOUT_1UA   0x2
#define AD74416H_BURNOUT_10UA  0x3

/* GPIO controller line numbers (adi,ad74416h-gpio) */
#define AD74416H_GPIO_A       0
#define AD74416H_GPIO_B       1
#define AD74416H_GPIO_C       2
#define AD74416H_GPIO_D       3
#define AD74416H_GPIO_E       4
#define AD74416H_GPIO_F       5
/* Debounced digital input comparator of channel ch (0..3), input only */
#define AD74416H_GPIO_DIN(ch) (6 + (ch))
/* External FET digital output of channel ch (0..3), output only */
#define AD74416H_GPIO_DO(ch)  (10 + (ch))

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_MFD_AD74416H_H_ */
