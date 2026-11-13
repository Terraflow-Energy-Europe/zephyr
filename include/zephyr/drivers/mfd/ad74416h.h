/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Analog Devices AD74416H quad-channel software configurable input/output.
 *
 * The core driver (adi,ad74416h) owns the SPI link and the device state. The
 * ADC, DAC and GPIO child drivers expose the standard Zephyr APIs on top of
 * it. This header provides everything else in the register map: channel
 * functions, digital input and output, burnout currents, alerts, watchdog,
 * and unit conversion helpers.
 *
 * Register and field names follow the AD74416H data sheet, Rev. 0.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MFD_AD74416H_H_
#define ZEPHYR_INCLUDE_DRIVERS_MFD_AD74416H_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/dt-bindings/mfd/ad74416h.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup mfd_interface_ad74416h MFD AD74416H Interface
 * @ingroup mfd_interfaces
 * @{
 */

/** Number of I/O channels (A to D) */
#define AD74416H_NUM_CHANNELS 4
/** Number of diagnostic ADC slots */
#define AD74416H_NUM_DIAGS    4
/** Number of GPIO pins (GPIO_A to GPIO_F) */
#define AD74416H_NUM_GPIOS    6

/** GENERIC_ID value of the AD74416H */
#define AD74416H_GENERIC_ID_VALUE 6

/** Sense resistor in series with each channel, in milliohms */
#define AD74416H_RSENSE_MOHM   12000
/** RTD reference resistance (SENSEHF filter + RSENSE), in milliohms */
#define AD74416H_RTD_RREF_MOHM 2012000

/**
 * @name Register map
 * @{
 */
#define AD74416H_REG_NOP                      0x00
#define AD74416H_REG_CH_FUNC_SETUP(ch)        (0x01 + 12 * (ch))
#define AD74416H_REG_ADC_CONFIG(ch)           (0x02 + 12 * (ch))
#define AD74416H_REG_DIN_CONFIG0(ch)          (0x03 + 12 * (ch))
#define AD74416H_REG_DIN_CONFIG1(ch)          (0x04 + 12 * (ch))
#define AD74416H_REG_OUTPUT_CONFIG(ch)        (0x05 + 12 * (ch))
#define AD74416H_REG_RTD_CONFIG(ch)           (0x06 + 12 * (ch))
#define AD74416H_REG_FET_LKG_COMP(ch)         (0x07 + 12 * (ch))
#define AD74416H_REG_DO_EXT_CONFIG(ch)        (0x08 + 12 * (ch))
#define AD74416H_REG_I_BURNOUT_CONFIG(ch)     (0x09 + 12 * (ch))
#define AD74416H_REG_DAC_CODE(ch)             (0x0A + 12 * (ch))
#define AD74416H_REG_DAC_ACTIVE(ch)           (0x0C + 12 * (ch))
#define AD74416H_REG_GPIO_CONFIG(n)           (0x32 + (n))
#define AD74416H_REG_PWR_OPTIM_CONFIG         0x38
#define AD74416H_REG_ADC_CONV_CTRL            0x39
#define AD74416H_REG_DIAG_ASSIGN              0x3A
#define AD74416H_REG_WDT_CONFIG               0x3B
#define AD74416H_REG_DIN_COMP_OUT             0x3E
#define AD74416H_REG_ALERT_STATUS             0x3F
#define AD74416H_REG_LIVE_STATUS              0x40
#define AD74416H_REG_ADC_RESULT_UPR(ch)       (0x41 + 2 * (ch))
#define AD74416H_REG_ADC_RESULT(ch)           (0x42 + 2 * (ch))
#define AD74416H_REG_ADC_DIAG_RESULT(n)       (0x49 + (n))
#define AD74416H_REG_LAST_ADC_RESULT_UPR      0x4D
#define AD74416H_REG_LAST_ADC_RESULT          0x4E
#define AD74416H_REG_DIN_COUNTER_UPR(ch)      (0x4F + 2 * (ch))
#define AD74416H_REG_DIN_COUNTER(ch)          (0x50 + 2 * (ch))
#define AD74416H_REG_SUPPLY_ALERT_STATUS      0x57
#define AD74416H_REG_CHANNEL_ALERT_STATUS(ch) (0x58 + (ch))
#define AD74416H_REG_ALERT_MASK               0x5C
#define AD74416H_REG_SUPPLY_ALERT_MASK        0x5D
#define AD74416H_REG_CHANNEL_ALERT_MASK(ch)   (0x5E + (ch))
#define AD74416H_REG_READ_SELECT              0x6E
#define AD74416H_REG_BURST_READ_SEL           0x6F
#define AD74416H_REG_THERM_RST                0x73
#define AD74416H_REG_CMD_KEY                  0x74
#define AD74416H_REG_BROADCAST_CMD_KEY        0x75
#define AD74416H_REG_SCRATCH(n)               (0x76 + (n))
#define AD74416H_REG_GENERIC_ID               0x7A
#define AD74416H_REG_SILICON_REV              0x7B
#define AD74416H_REG_SILICON_ID0              0x7D
#define AD74416H_REG_SILICON_ID1              0x7E
/* HART modem registers. No driver API yet; reachable through ad74416h_reg_*() */
#define AD74416H_REG_HART_ALERT_STATUS(ch)    (0x80 + 16 * (ch))
#define AD74416H_REG_HART_RX(ch)              (0x81 + 16 * (ch))
#define AD74416H_REG_HART_TX(ch)              (0x82 + 16 * (ch))
#define AD74416H_REG_HART_FCR(ch)             (0x83 + 16 * (ch))
#define AD74416H_REG_HART_MCR(ch)             (0x84 + 16 * (ch))
#define AD74416H_REG_HART_RFC(ch)             (0x85 + 16 * (ch))
#define AD74416H_REG_HART_TFC(ch)             (0x86 + 16 * (ch))
#define AD74416H_REG_HART_ALERT_MASK(ch)      (0x87 + 16 * (ch))
#define AD74416H_REG_HART_CONFIG(ch)          (0x88 + 16 * (ch))
#define AD74416H_REG_HART_TX_PREM(ch)         (0x89 + 16 * (ch))
#define AD74416H_REG_HART_EVDET(ch)           (0x8A + 16 * (ch))
#define AD74416H_REG_HART_CH_RESET(ch)        (0x8C + 16 * (ch))
#define AD74416H_REG_HART_GPIO_IF_CONFIG      0xC0
#define AD74416H_REG_HART_GPIO_MON_CONFIG(n)  (0xC1 + (n))
/** Highest register address */
#define AD74416H_REG_MAX                      0xC4
/** @} */

/**
 * @name Register fields
 * @{
 */
/* CH_FUNC_SETUPn */
#define AD74416H_CH_FUNC GENMASK(3, 0)

/* ADC_CONFIGn */
#define AD74416H_ADC_CONV_RATE  GENMASK(11, 8)
#define AD74416H_ADC_CONV_RANGE GENMASK(6, 4)
#define AD74416H_ADC_CONV_MUX   GENMASK(2, 0)

/* DIN_CONFIG0n */
#define AD74416H_DIN_COUNT_EN      BIT(15)
#define AD74416H_DIN_INV_COMP_OUT  BIT(14)
#define AD74416H_DIN_COMPARATOR_EN BIT(13)
#define AD74416H_DIN_SINK_RANGE    BIT(12)
#define AD74416H_DIN_SINK          GENMASK(11, 7)
#define AD74416H_DIN_DEBOUNCE_MODE BIT(6)
#define AD74416H_DIN_DEBOUNCE_TIME GENMASK(4, 0)

/* DIN_CONFIG1n */
#define AD74416H_DIN_INPUT_SELECT    BIT(10)
#define AD74416H_DIN_SC_DET_EN       BIT(9)
#define AD74416H_DIN_OC_DET_EN       BIT(8)
#define AD74416H_DIN_THRESH_MODE     BIT(7)
#define AD74416H_DIN_COMP_THRESH     GENMASK(6, 0)
/** Highest valid COMP_THRESH code */
#define AD74416H_DIN_COMP_THRESH_MAX 98

/* OUTPUT_CONFIGn */
#define AD74416H_OUT_AVDD_SELECT        GENMASK(15, 14)
#define AD74416H_OUT_ALARM_DEG_PERIOD   BIT(12)
#define AD74416H_OUT_VOUT_4W_EN         BIT(11)
#define AD74416H_OUT_WAIT_LDAC_CMD      BIT(10)
#define AD74416H_OUT_VIOUT_DRV_EN_DLY   BIT(9)
#define AD74416H_OUT_HART_COMPL_SETTLED BIT(8)
#define AD74416H_OUT_VOUT_RANGE         BIT(7)
#define AD74416H_OUT_SLEW_EN            GENMASK(6, 5)
#define AD74416H_OUT_SLEW_LIN_STEP      GENMASK(4, 3)
#define AD74416H_OUT_SLEW_LIN_RATE      GENMASK(2, 1)
#define AD74416H_OUT_I_LIMIT            BIT(0)

/* RTD_CONFIGn */
#define AD74416H_RTD_ADC_REF  BIT(3)
#define AD74416H_RTD_MODE_SEL BIT(2)
#define AD74416H_RTD_EXC_SWAP BIT(1)
#define AD74416H_RTD_CURRENT  BIT(0)

/* FET_LKG_COMPn */
#define AD74416H_FET_SRC_LKG_COMP_EN BIT(0)

/* DO_EXT_CONFIGn */
#define AD74416H_DO_T2      GENMASK(12, 8)
#define AD74416H_DO_DATA    BIT(7)
#define AD74416H_DO_T1      GENMASK(6, 2)
#define AD74416H_DO_SRC_SEL BIT(1)
#define AD74416H_DO_MODE    BIT(0)

/* I_BURNOUT_CONFIGn */
#define AD74416H_BRN_SEN_VSENSEN_CURR GENMASK(6, 5)
#define AD74416H_BRN_SEN_VSENSEN_POL  BIT(4)
#define AD74416H_BRN_VIOUT_CURR       GENMASK(2, 1)
#define AD74416H_BRN_VIOUT_POL        BIT(0)

/* GPIO_CONFIGn */
#define AD74416H_GPIO_DIN_DO_CH   GENMASK(7, 6)
#define AD74416H_GPIO_GPI_DATA    BIT(5)
#define AD74416H_GPIO_GPO_DATA    BIT(4)
#define AD74416H_GPIO_GP_WK_PD_EN BIT(3)
#define AD74416H_GPIO_SELECT      GENMASK(2, 0)
#define AD74416H_GPIO_SEL_HIGH_Z  0x0
#define AD74416H_GPIO_SEL_GPO     0x1
#define AD74416H_GPIO_SEL_GPI     0x2
#define AD74416H_GPIO_SEL_DIN     0x3
#define AD74416H_GPIO_SEL_DO      0x4

/* PWR_OPTIM_CONFIG */
#define AD74416H_PWR_REF_EN           BIT(13)
#define AD74416H_PWR_SENSE_AGND_OPT   BIT(12)
#define AD74416H_PWR_SENSE_HF_OPT(ch) BIT(8 + (ch))
#define AD74416H_PWR_SENSE_LF_OPT(ch) BIT(4 + (ch))
#define AD74416H_PWR_VSENSEN_OPT(ch)  BIT(ch)

/* ADC_CONV_CTRL */
#define AD74416H_CONV_ADC_RDY_CTRL   BIT(13)
#define AD74416H_CONV_RATE_DIAG      GENMASK(12, 10)
#define AD74416H_CONV_SEQ            GENMASK(9, 8)
#define AD74416H_CONV_DIAG_EN(n)     BIT(4 + (n))
#define AD74416H_CONV_DIAG_EN_ALL    GENMASK(7, 4)
#define AD74416H_CONV_CH_EN(ch)      BIT(ch)
#define AD74416H_CONV_CH_EN_ALL      GENMASK(3, 0)
#define AD74416H_CONV_SEQ_IDLE       0x0
#define AD74416H_CONV_SEQ_SINGLE     0x1
#define AD74416H_CONV_SEQ_CONTINUOUS 0x2
#define AD74416H_CONV_SEQ_POWER_DOWN 0x3

/* DIAG_ASSIGN */
#define AD74416H_DIAG_ASSIGN(n) (GENMASK(3, 0) << (4 * (n)))

/* WDT_CONFIG */
#define AD74416H_WDT_EN      BIT(4)
#define AD74416H_WDT_TIMEOUT GENMASK(3, 0)

/* DIN_COMP_OUT */
#define AD74416H_DIN_COMP_OUT(ch) BIT(ch)

/* ALERT_STATUS and ALERT_MASK */
#define AD74416H_ALERT_HART(ch)       BIT(12 + (ch))
#define AD74416H_ALERT_CHANNEL(ch)    BIT(8 + (ch))
#define AD74416H_ALERT_ADC_ERR        BIT(5)
#define AD74416H_ALERT_TEMP           BIT(4)
#define AD74416H_ALERT_SPI_ERR        BIT(3)
#define AD74416H_ALERT_SUPPLY_ERR     BIT(2)
#define AD74416H_ALERT_RESET_OCCURRED BIT(0)

/* LIVE_STATUS */
#define AD74416H_LIVE_ANALOG_IO_STATUS(ch) BIT(12 + (ch))
#define AD74416H_LIVE_DO_STATUS(ch)        BIT(8 + (ch))
#define AD74416H_LIVE_DIN_STATUS(ch)       BIT(4 + (ch))
#define AD74416H_LIVE_TEMP_ALERT_STATUS    BIT(3)
#define AD74416H_LIVE_ADC_DATA_RDY         BIT(2)
#define AD74416H_LIVE_ADC_BUSY             BIT(1)
#define AD74416H_LIVE_SUPPLY_STATUS        BIT(0)

/* ADC_RESULT_UPRn */
#define AD74416H_RES_CONV_RES_MUX   GENMASK(15, 13)
#define AD74416H_RES_CONV_RES_RANGE GENMASK(12, 10)
#define AD74416H_RES_CONV_SEQ_COUNT GENMASK(9, 8)
#define AD74416H_RES_CONV_RES_UPR   GENMASK(7, 0)

/* LAST_ADC_RESULT_UPR */
#define AD74416H_LAST_CONV_CH      GENMASK(10, 8)
#define AD74416H_LAST_CONV_RES_UPR GENMASK(7, 0)

/* SUPPLY_ALERT_STATUS and SUPPLY_ALERT_MASK */
#define AD74416H_SUPPLY_AVDD_HI_ERR BIT(6)
#define AD74416H_SUPPLY_AVDD_LO_ERR BIT(5)
#define AD74416H_SUPPLY_DO_VDD_ERR  BIT(4)
#define AD74416H_SUPPLY_AVCC_ERR    BIT(3)
#define AD74416H_SUPPLY_DVCC_ERR    BIT(2)
#define AD74416H_SUPPLY_AVSS_ERR    BIT(1)
#define AD74416H_SUPPLY_CAL_MEM_ERR BIT(0)
#define AD74416H_SUPPLY_ALL         GENMASK(6, 0)

/* CHANNEL_ALERT_STATUSn and CHANNEL_ALERT_MASKn */
#define AD74416H_CH_ALERT_VIOUT_SHUTDOWN BIT(6)
#define AD74416H_CH_ALERT_ANALOG_IO_OC   BIT(5)
#define AD74416H_CH_ALERT_ANALOG_IO_SC   BIT(4)
#define AD74416H_CH_ALERT_DO_TIMEOUT     BIT(3)
#define AD74416H_CH_ALERT_DO_SC          BIT(2)
#define AD74416H_CH_ALERT_DIN_OC         BIT(1)
#define AD74416H_CH_ALERT_DIN_SC         BIT(0)
#define AD74416H_CH_ALERT_ALL            GENMASK(6, 0)

/* READ_SELECT */
#define AD74416H_READBACK_ADDR GENMASK(8, 0)

/* BURST_READ_SEL */
#define AD74416H_BURST_ALERT_STATUS        BIT(0)
#define AD74416H_BURST_LIVE_STATUS         BIT(1)
#define AD74416H_BURST_ADC_RESULT(ch)      BIT(2 + (ch))
#define AD74416H_BURST_ADC_DIAG(n)         BIT(6 + (n))
#define AD74416H_BURST_LAST_ADC_RESULT     BIT(10)
#define AD74416H_BURST_DIN_COUNTER(ch)     BIT(11 + (ch))
#define AD74416H_BURST_SUPPLY_ALERT_STATUS BIT(15)

/* THERM_RST */
#define AD74416H_THERM_RST_EN BIT(0)

/* CMD_KEY and BROADCAST_CMD_KEY values */
#define AD74416H_CMD_SW_RESET_KEY1   0x15FA
#define AD74416H_CMD_SW_RESET_KEY2   0xAF51
#define AD74416H_CMD_DAC_UPDATE      0x1C7D
#define AD74416H_BCAST_SW_RESET_KEY1 0x1A78
#define AD74416H_BCAST_SW_RESET_KEY2 0xD203
#define AD74416H_BCAST_DAC_UPDATE    0x964E

/* GENERIC_ID, SILICON_REV, SILICON_IDn */
#define AD74416H_GENERIC_ID     GENMASK(2, 0)
#define AD74416H_SILICON_REV_ID GENMASK(7, 0)
#define AD74416H_SILICON_UID0   GENMASK(6, 0)
#define AD74416H_SILICON_UID2   GENMASK(11, 6)
#define AD74416H_SILICON_UID1   GENMASK(5, 0)

/* HART_ALERT_STATUSn and HART_ALERT_MASKn */
#define AD74416H_HART_FRM_MON_STATE   GENMASK(15, 13)
#define AD74416H_HART_EOM             BIT(12)
#define AD74416H_HART_RX_BCNT         BIT(11)
#define AD74416H_HART_RX_CMD          BIT(10)
#define AD74416H_HART_SOM             BIT(9)
#define AD74416H_HART_CD              BIT(8)
#define AD74416H_HART_CD_EDGE_DET     BIT(7)
#define AD74416H_HART_TX_COMPLETE     BIT(6)
#define AD74416H_HART_TX_FIFO_ALERT   BIT(5)
#define AD74416H_HART_RX_FIFO_ALERT   BIT(4)
#define AD74416H_HART_RX_OVERFLOW_ERR BIT(3)
#define AD74416H_HART_FRAME_ERR       BIT(2)
#define AD74416H_HART_PARITY_ERR      BIT(1)
#define AD74416H_HART_GAP_ERR         BIT(0)
/** HART_ALERT_STATUSn bits that are write-1-to-clear */
#define AD74416H_HART_ALERT_W1C       (GENMASK(12, 9) | BIT(7) | BIT(6) | GENMASK(3, 0))

/* HART_RXn */
#define AD74416H_HART_RFGI BIT(11)
#define AD74416H_HART_RFFE BIT(10)
#define AD74416H_HART_RFPE BIT(9)
#define AD74416H_HART_RFBI BIT(8)
#define AD74416H_HART_RBR  GENMASK(7, 0)

/* HART_FCRn */
#define AD74416H_HART_TFTRIG GENMASK(12, 8)
#define AD74416H_HART_RFTRIG GENMASK(7, 3)
#define AD74416H_HART_TFCLR  BIT(2)
#define AD74416H_HART_RFCLR  BIT(1)
#define AD74416H_HART_FIFOEN BIT(0)

/* HART_MCRn */
#define AD74416H_HART_RTS BIT(0)

/* HART_RFCn, HART_TFCn */
#define AD74416H_HART_FIFO_COUNT GENMASK(5, 0)

/* HART_CONFIGn */
#define AD74416H_HART_CD_EXTD_QUAL      BIT(13)
#define AD74416H_HART_FRM_MON_RX_PREMX2 BIT(12)
#define AD74416H_HART_FRM_MON_RST_GAP   BIT(11)
#define AD74416H_HART_FRM_MON_RST_CD    BIT(10)
#define AD74416H_HART_RX_ALL_CHARS      BIT(9)
#define AD74416H_HART_FRM_MON_EN        BIT(8)
#define AD74416H_HART_EVENT_DET_SEL     GENMASK(7, 6)
#define AD74416H_HART_TX_1B_AFTER_RTS   BIT(5)
#define AD74416H_HART_AUTO_CLR_RTS      BIT(4)
#define AD74416H_HART_CD_EDGE_SEL       GENMASK(3, 2)
#define AD74416H_HART_MODEM_DUPLEX      BIT(1)
#define AD74416H_HART_MODEM_PWRUP       BIT(0)

/* HART_TX_PREMn */
#define AD74416H_HART_TX_PREM_CNT GENMASK(4, 0)

/* HART_CH_RESETn */
#define AD74416H_HART_HRST BIT(0)

/* HART_GPIO_IF_CONFIG */
#define AD74416H_HART_GPIO_IF_CH  GENMASK(3, 2)
#define AD74416H_HART_GPIO_IF_SEL GENMASK(1, 0)

/* HART_GPIO_MON_CONFIGn */
#define AD74416H_HART_GPIO_MON_CH  GENMASK(4, 3)
#define AD74416H_HART_GPIO_MON_SEL GENMASK(2, 0)
/** @} */

/**
 * @name Timing, from the data sheet
 * @{
 */
/** Device reset time, until the calibration memory is loaded */
#define AD74416H_RESET_TIME_US        1000
/** Minimum RESET pin pulse width */
#define AD74416H_RESET_PULSE_US       50
/** Channel initialization time for all functions except IOUT_HART */
#define AD74416H_CH_INIT_TIME_US      300
/** Channel initialization time for IOUT_HART */
#define AD74416H_CH_INIT_TIME_HART_US 4200
/** @} */

/**
 * @brief Full configuration of one channel.
 *
 * Fields set to -1 keep the default that the device loads when the channel
 * function is selected (data sheet Table 22). Use
 * ad74416h_channel_config_init() to get a configuration with every field at
 * its default.
 */
struct ad74416h_channel_config {
	/** AD74416H_CH_FUNC_* */
	uint8_t func;

	/* Output stage (OUTPUT_CONFIGn), applied in high impedance */
	bool vout_bipolar;           /**< VOUT range -12 V to +12 V instead of 0 V to 12 V */
	bool vout_4wire;             /**< 4-wire VOUT feedback sensing */
	bool i_limit_8ma;            /**< VOUT current limit 8 mA instead of 16 mA */
	bool alarm_deglitch_20ms;    /**< VOUT/IOUT alarm deglitch 20 ms instead of 4 ms */
	bool viout_drv_en_dly_200us; /**< IOUT_HART driver enable delay 200 us instead of 4 ms */
	int8_t avdd_select;          /**< AD74416H_AVDD_*, or -1 */
	uint8_t slew;                /**< AD74416H_SLEW_* */
	uint8_t slew_step;           /**< AD74416H_SLEW_STEP_* */
	uint8_t slew_rate;           /**< AD74416H_SLEW_RATE_* */
	/** DAC code written before the function is enabled, or -1 for 0 V/0 mA */
	int32_t dac_code;

	/* ADC (ADC_CONFIGn) */
	int8_t adc_range; /**< AD74416H_ADC_RANGE_*, or -1 */
	int8_t adc_mux;   /**< AD74416H_ADC_MUX_*, or -1 */
	uint8_t adc_rate; /**< AD74416H_ADC_RATE_* */

	/* Resistance measurement (RTD_CONFIGn) */
	bool rtd_2wire;    /**< 2-wire instead of 3-wire */
	bool rtd_500ua;    /**< 500 uA excitation instead of 1 mA */
	bool rtd_ref_1v;   /**< 1 V reference instead of 2 V */
	bool rtd_exc_swap; /**< Swap excitation currents (3-wire) */

	/* Digital input (DIN_CONFIG0n, DIN_CONFIG1n) */
	int8_t din_comparator;     /**< 1 enable, 0 disable, -1 function default */
	int8_t din_invert;         /**< 1 invert, 0 normal, -1 function default */
	int8_t din_threshold;      /**< COMP_THRESH code 0..98, or -1 */
	bool din_threshold_fixed;  /**< Fixed threshold instead of AVDD_HI scaled */
	uint8_t din_sink;          /**< Sink current code 0..31, 0 is off */
	bool din_sink_range_high;  /**< 240 uA steps instead of 120 uA */
	uint8_t din_debounce_time; /**< DEBOUNCE_TIME code 0..31, 0 bypasses */
	bool din_debounce_mode1;   /**< Debounce mode 1 instead of integrator */
	bool din_counter;          /**< Count debounced edges */
	bool din_oc_detect;        /**< Open-circuit detection */
	bool din_sc_detect;        /**< Short-circuit detection */
	bool din_input_viout;      /**< Unbuffered VIOUT input instead of VSENSEP */

	/* External FET digital output (DO_EXT_CONFIGn) */
	bool do_enable;        /**< Enable the sourcing digital output */
	bool do_gpio_control;  /**< FET driven by a GPIO pin instead of DO_DATA */
	uint8_t do_t1;         /**< DO_T1 code 0..31 */
	uint8_t do_t2;         /**< DO_T2 code 0..31 */
	bool fet_leakage_comp; /**< FET leakage compensation (DO disabled only) */

	/* Burnout currents (I_BURNOUT_CONFIGn) */
	uint8_t burnout_viout;       /**< AD74416H_BURNOUT_* on VIOUT */
	bool burnout_viout_source;   /**< Source instead of sink */
	uint8_t burnout_vsensen;     /**< AD74416H_BURNOUT_* on VSENSEN */
	bool burnout_vsensen_source; /**< Source instead of sink */
};

/** Alert registers, as read by ad74416h_alerts_get() */
struct ad74416h_alerts {
	uint16_t alert;                          /**< ALERT_STATUS */
	uint16_t supply;                         /**< SUPPLY_ALERT_STATUS */
	uint16_t channel[AD74416H_NUM_CHANNELS]; /**< CHANNEL_ALERT_STATUSn */
	uint16_t hart[AD74416H_NUM_CHANNELS];    /**< HART_ALERT_STATUSn */
};

/**
 * @brief Alert callback, called from the system work queue after the ALERT
 *        pin asserts. Alerts are not cleared automatically.
 */
typedef void (*ad74416h_alert_cb_t)(const struct device *dev, const struct ad74416h_alerts *alerts,
				    void *user_data);

/**
 * @name Register access
 *
 * All functions in this header are thread safe. Use ad74416h_lock() to make
 * a sequence of calls atomic.
 * @{
 */

/**
 * @brief Read a register (two-stage readback, CRC checked).
 *
 * @param dev AD74416H device
 * @param reg Register address
 * @param val Register value
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_reg_read(const struct device *dev, uint8_t reg, uint16_t *val);

/**
 * @brief Read several registers back to back.
 *
 * Each read stage also selects the next register, so a register pair such as
 * ADC_RESULT_UPRn/ADC_RESULTn is read without any other read in between, as
 * the data sheet requires.
 *
 * @param dev   AD74416H device
 * @param regs  Register addresses
 * @param vals  Register values
 * @param count Number of registers
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_reg_read_multi(const struct device *dev, const uint8_t *regs, uint16_t *vals,
			    size_t count);

/**
 * @brief Write a register.
 *
 * @param dev AD74416H device
 * @param reg Register address
 * @param val Value to write
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_reg_write(const struct device *dev, uint8_t reg, uint16_t val);

/**
 * @brief Read-modify-write the bits in @p mask.
 *
 * @param dev  AD74416H device
 * @param reg  Register address
 * @param mask Bits to change
 * @param val  New value of the bits in @p mask
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_reg_update(const struct device *dev, uint8_t reg, uint16_t mask, uint16_t val);

/**
 * @brief Lock the device for a sequence of calls. Recursive.
 *
 * @param dev AD74416H device
 */
void ad74416h_lock(const struct device *dev);

/**
 * @brief Unlock the device.
 *
 * @param dev AD74416H device
 */
void ad74416h_unlock(const struct device *dev);

/**
 * @brief Get the status bits of the last read frame.
 *
 * Status bits D35:D32: ALERT (bit 3), HART_ALERT (bit 2), CHANNEL_ALERT
 * (bit 1), ADC_RDY (bit 0).
 *
 * @param dev AD74416H device
 *
 * @return Status bits
 */
uint8_t ad74416h_last_status(const struct device *dev);
/** @} */

/**
 * @name Device control
 * @{
 */

/**
 * @brief Reset the device.
 *
 * Uses the RESET pin if wired, otherwise the software reset keys. Waits for
 * the calibration memory to load, clears the alerts and applies the
 * devicetree configuration again.
 *
 * @param dev AD74416H device
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_reset(const struct device *dev);

/**
 * @brief Read the silicon revision and unique ID.
 *
 * @param dev          AD74416H device
 * @param silicon_rev  Silicon revision
 * @param unique_id    Unique ID
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_get_id(const struct device *dev, uint8_t *silicon_rev, uint32_t *unique_id);

/**
 * @brief Enable the SPI watchdog.
 *
 * The device resets, putting every channel in high impedance, if no valid SPI
 * frame arrives within the timeout.
 *
 * @param dev          AD74416H device
 * @param timeout_code WDT_TIMEOUT code 0x0 (1 ms) .. 0xA (2 s).
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_wdt_enable(const struct device *dev, uint8_t timeout_code);

/**
 * @brief Disable the SPI watchdog.
 *
 * @param dev AD74416H device
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_wdt_disable(const struct device *dev);

/**
 * @brief Enable or disable the thermal reset at about 140 degC.
 *
 * @param dev    AD74416H device
 * @param enable True to enable the thermal reset
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_thermal_reset_enable(const struct device *dev, bool enable);

/**
 * @brief Put sense buffers in low power standby (PWR_OPTIM_CONFIG).
 *
 * Do not call during an ADC conversion sequence.
 *
 * @param dev              AD74416H device
 * @param pwr_optim_config PWR_OPTIM_CONFIG value
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_power_optim_set(const struct device *dev, uint16_t pwr_optim_config);

/**
 * @brief Write the DAC update key, loading DAC codes held by WAIT_LDAC_CMD.
 *
 * @param dev AD74416H device
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_dac_update_all(const struct device *dev);
/** @} */

/**
 * @name Channel functions
 * @{
 */

/**
 * @brief Fill @p cfg with the defaults: high impedance, nothing overridden.
 *
 * @param cfg Channel configuration to initialize
 */
void ad74416h_channel_config_init(struct ad74416h_channel_config *cfg);

/**
 * @brief Configure a channel.
 *
 * Follows the data sheet sequence: disable any digital output, go to high
 * impedance, set the output range and DAC code, wait 300 us, select the new
 * function, apply the remaining settings, and wait the channel
 * initialization time (300 us, or 4.2 ms for IOUT_HART).
 *
 * @param dev AD74416H device
 * @param ch  Channel number
 * @param cfg Channel configuration
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_channel_configure(const struct device *dev, uint8_t ch,
			       const struct ad74416h_channel_config *cfg);

/**
 * @brief Get the configuration last applied to a channel.
 *
 * @param dev AD74416H device
 * @param ch  Channel number
 * @param cfg Channel configuration
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_channel_config_get(const struct device *dev, uint8_t ch,
				struct ad74416h_channel_config *cfg);

/**
 * @brief Get the current function of a channel (AD74416H_CH_FUNC_*).
 *
 * @param dev  AD74416H device
 * @param ch   Channel number
 * @param func Channel function
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_channel_func_get(const struct device *dev, uint8_t ch, uint8_t *func);
/** @} */

/**
 * @name ADC
 *
 * Results are raw codes: 24 bit for channels, 16 bit for diagnostics. Use
 * the conversion helpers below to get physical units.
 * @{
 */

/**
 * @brief Set the conversion range, rate and input of a channel.
 *
 * @param dev   AD74416H device
 * @param ch    Channel number
 * @param range AD74416H_ADC_RANGE_*
 * @param rate  AD74416H_ADC_RATE_*
 * @param mux   AD74416H_ADC_MUX_*
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_adc_channel_set(const struct device *dev, uint8_t ch, uint8_t range, uint8_t rate,
			     uint8_t mux);

/**
 * @brief Get the conversion range of a channel.
 *
 * @param dev   AD74416H device
 * @param ch    Channel number
 * @param range Conversion range
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_adc_range_get(const struct device *dev, uint8_t ch, uint8_t *range);

/**
 * @brief Assign a source (AD74416H_DIAG_*) to a diagnostic slot.
 *
 * @param dev  AD74416H device
 * @param slot Diagnostic slot
 * @param diag AD74416H_DIAG_*
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_diag_assign(const struct device *dev, uint8_t slot, uint8_t diag);

/**
 * @brief Get the source assigned to a diagnostic slot.
 *
 * @param dev  AD74416H device
 * @param slot Diagnostic slot
 * @param diag Assigned source
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_diag_get(const struct device *dev, uint8_t slot, uint8_t *diag);

/**
 * @brief Set the diagnostic conversion rate (AD74416H_DIAG_RATE_*).
 *
 * @param dev  AD74416H device
 * @param rate AD74416H_DIAG_RATE_*
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_diag_rate_set(const struct device *dev, uint8_t rate);

/**
 * @brief Run one conversion sequence and read the results.
 *
 * @param dev       AD74416H device
 * @param ch_mask   Channels to convert, bit n is channel n.
 * @param diag_mask Diagnostic slots to convert, bit n is slot n.
 * @param results   Results, index 0..3 channels and 4..7 diagnostics. Only
 *                  the requested entries are written.
 * @param timeout   Time to wait for the sequence to complete.
 *
 * @retval 0 on success
 * @retval -EBUSY if continuous conversions are running
 * @retval -ETIMEDOUT if the sequence did not complete in time
 * @retval -errno on other failure
 */
int ad74416h_adc_convert(const struct device *dev, uint8_t ch_mask, uint8_t diag_mask,
			 uint32_t results[8], k_timeout_t timeout);

/**
 * @brief Start continuous conversions.
 *
 * Read results with ad74416h_adc_results_read() after each sequence (ADC_RDY
 * pin or ADC_DATA_RDY).
 *
 * @param dev       AD74416H device
 * @param ch_mask   Channels to convert, bit n is channel n.
 * @param diag_mask Diagnostic slots to convert, bit n is slot n.
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_adc_start_continuous(const struct device *dev, uint8_t ch_mask, uint8_t diag_mask);

/**
 * @brief Stop conversions.
 *
 * @param dev        AD74416H device
 * @param power_down Power the ADC down instead of idling
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_adc_stop(const struct device *dev, bool power_down);

/**
 * @brief Read the latest results of a continuous sequence.
 *
 * @param dev       AD74416H device
 * @param ch_mask   Channels to read, bit n is channel n.
 * @param diag_mask Diagnostic slots to read, bit n is slot n.
 * @param results   Results, index 0..3 channels and 4..7 diagnostics.
 * @param seq_count CONV_SEQ_COUNT of the results, may be NULL.
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_adc_results_read(const struct device *dev, uint8_t ch_mask, uint8_t diag_mask,
			      uint32_t results[8], uint8_t *seq_count);

/**
 * @brief Input voltage for an ADC code, in nanovolts.
 *
 * @param range AD74416H_ADC_RANGE_*
 * @param code  24-bit code
 *
 * @return Nanovolts
 */
int64_t ad74416h_adc_code_to_nv(uint8_t range, uint32_t code);

/**
 * @brief Current through RSENSE for an ADC code, in nanoamps.
 *
 * The sign follows the voltage from SENSEHF to SENSELF.
 *
 * @param range       AD74416H_ADC_RANGE_*
 * @param code        24-bit code
 * @param rsense_mohm Sense resistor in milliohms, AD74416H_RSENSE_MOHM
 *                    unless calibrated.
 *
 * @return Nanoamps
 */
int64_t ad74416h_adc_code_to_na(uint8_t range, uint32_t code, uint32_t rsense_mohm);

/**
 * @brief RTD resistance for a resistance measurement code, in milliohms.
 *
 * @param range AD74416H_ADC_RANGE_* used for the measurement.
 * @param code  24-bit code
 *
 * @return Milliohms
 */
int64_t ad74416h_rtd_code_to_mohm(uint8_t range, uint32_t code);

/**
 * @brief Value of a diagnostic result (data sheet Table 30).
 *
 * @param diag      AD74416H_DIAG_* assigned to the slot.
 * @param code      16-bit diagnostic code.
 * @param aux       For AD74416H_DIAG_VSENSEP: AVDD_HI in microvolts when
 *                  DIN_THRESH_MODE is 0, or INT32_MIN when it is 1.
 *                  For AD74416H_DIAG_I_DO_SRC: RSET in milliohms.
 *                  Ignored otherwise.
 *
 * @return Microvolts, milli-degrees Celsius for AD74416H_DIAG_TEMP, or
 *         microamps for AD74416H_DIAG_I_DO_SRC.
 */
int64_t ad74416h_diag_code_to_value(uint8_t diag, uint16_t code, int32_t aux);
/** @} */

/**
 * @name DAC
 * @{
 */

/**
 * @brief Set the DAC code of a channel.
 *
 * Returns -EBUSY while linear slewing is still moving towards the previous
 * code, as the data sheet forbids changing it.
 *
 * @param dev  AD74416H device
 * @param ch   Channel number
 * @param code DAC code
 *
 * @retval 0 on success
 * @retval -EBUSY if slewing is still in progress
 * @retval -errno on other failure
 */
int ad74416h_dac_set(const struct device *dev, uint8_t ch, uint16_t code);

/**
 * @brief Get the code currently driven by the DAC (DAC_ACTIVEn).
 *
 * @param dev  AD74416H device
 * @param ch   Channel number
 * @param code DAC code
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_dac_active_get(const struct device *dev, uint8_t ch, uint16_t *code);

/**
 * @brief Configure DAC slewing (AD74416H_SLEW_*).
 *
 * For AD74416H_SLEW_HART this waits for HART_COMPL_SETTLED first.
 *
 * @param dev  AD74416H device
 * @param ch   Channel number
 * @param slew AD74416H_SLEW_*
 * @param step Slew step
 * @param rate Slew rate
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_dac_slew_set(const struct device *dev, uint8_t ch, uint8_t slew, uint8_t step,
			  uint8_t rate);

/**
 * @brief DAC code for a voltage output in microvolts.
 *
 * @param bipolar True for the bipolar output range
 * @param uv      Voltage in microvolts
 * @param code    DAC code
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_dac_uv_to_code(bool bipolar, int32_t uv, uint16_t *code);

/**
 * @brief DAC code for a current output (0 to 25 mA) in microamps.
 *
 * @param ua   Current in microamps
 * @param code DAC code
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_dac_ua_to_code(int32_t ua, uint16_t *code);
/** @} */

/**
 * @name Digital input and output
 * @{
 */

/**
 * @brief Read the debounced comparator outputs, bit n is channel n.
 *
 * @param dev   AD74416H device
 * @param state Comparator outputs
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_din_get(const struct device *dev, uint8_t *state);

/**
 * @brief Read the 32-bit debounced edge counter of a channel.
 *
 * @param dev   AD74416H device
 * @param ch    Channel number
 * @param count Edge count
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_din_counter_get(const struct device *dev, uint8_t ch, uint32_t *count);

/**
 * @brief Set the digital input threshold code (COMP_THRESH) and mode.
 *
 * @param dev   AD74416H device
 * @param ch    Channel number
 * @param code  COMP_THRESH code
 * @param fixed True for a fixed threshold (DIN_THRESH_MODE = 1)
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_din_threshold_set(const struct device *dev, uint8_t ch, uint8_t code, bool fixed);

/**
 * @brief COMP_THRESH code for a fixed threshold voltage.
 *
 * For DIN_THRESH_MODE = 1, V = 2.5 V * (code - 38) / 5, so -19 V to +30 V.
 *
 * @param mv   Threshold in millivolts
 * @param code COMP_THRESH code
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_din_threshold_fixed_mv_to_code(int32_t mv, uint8_t *code);

/**
 * @brief COMP_THRESH code for a threshold that scales with AVDD_HI.
 *
 * For DIN_THRESH_MODE = 0, V = AVDD_HI * (code - 48) / 50.
 *
 * @param mv         Threshold in millivolts
 * @param avdd_hi_mv AVDD_HI in millivolts
 * @param code       COMP_THRESH code
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_din_threshold_avdd_mv_to_code(int32_t mv, int32_t avdd_hi_mv, uint8_t *code);

/**
 * @brief Switch the external digital output FET of a channel on or off.
 *
 * @param dev AD74416H device
 * @param ch  Channel number
 * @param on  True to switch on
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_do_set(const struct device *dev, uint8_t ch, bool on);

/**
 * @brief Re-enable a digital output after a DO_TIMEOUT.
 *
 * Clears DO_DATA, enables DO_MODE, then sets DO_DATA if @p on.
 *
 * @param dev AD74416H device
 * @param ch  Channel number
 * @param on  True to set DO_DATA
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_do_recover(const struct device *dev, uint8_t ch, bool on);
/** @} */

/**
 * @name Alerts
 * @{
 */

/**
 * @brief Read all alert registers.
 *
 * @param dev    AD74416H device
 * @param alerts Alert registers
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_alerts_get(const struct device *dev, struct ad74416h_alerts *alerts);

/**
 * @brief Clear the alerts set in @p alerts (write 1 to clear).
 *
 * The detail registers are cleared first and ALERT_STATUS last, as the data
 * sheet requires.
 *
 * @param dev    AD74416H device
 * @param alerts Alerts to clear
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_alerts_clear(const struct device *dev, const struct ad74416h_alerts *alerts);

/**
 * @brief Set the alert masks.
 *
 * A set bit keeps that alert off the ALERT pin.
 *
 * @param dev   AD74416H device
 * @param masks Alert masks
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_alerts_mask_set(const struct device *dev, const struct ad74416h_alerts *masks);

/**
 * @brief Read the live (unlatched) status (LIVE_STATUS).
 *
 * @param dev    AD74416H device
 * @param status LIVE_STATUS value
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_live_status_get(const struct device *dev, uint16_t *status);

/**
 * @brief Set the alert callback.
 *
 * Needs alert-gpios in devicetree. NULL removes it.
 *
 * @param dev       AD74416H device
 * @param cb        Callback, or NULL to remove it
 * @param user_data User data passed to the callback
 *
 * @retval 0 on success
 * @retval -errno on failure
 */
int ad74416h_alert_callback_set(const struct device *dev, ad74416h_alert_cb_t cb, void *user_data);
/** @} */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MFD_AD74416H_H_ */
